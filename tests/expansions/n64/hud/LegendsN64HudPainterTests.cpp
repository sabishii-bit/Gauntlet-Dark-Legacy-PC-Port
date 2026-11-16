#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "TestSupport.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64HudFixture.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/config/GameConfig.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Fixture = test::LegendsN64HudFixture;
using Layout = LegendsN64HudLayout;

/** Full health at the level the cases' characters stand at. */
constexpr s32 kMostHealth = 600;
/** A panel's top on the frame. */
constexpr f32 kTop = 170;

/** One quad of the recorded draws: where it lands, in the units it was drawn in; the part of
 * its texture across it, which runs backwards where the picture is turned; its colour; and the
 * transform it was drawn through. */
struct Quad {
    const Texture* texture = nullptr;
    Rect area;
    Rect uv;
    Color color;
    Mat4 transform{1.0f};
};

/** Every quad `device` recorded, in the order drawn. A quad's first vertex is its top left and
 * its third its bottom right (ImmediateBatch::rect). */
std::vector<Quad> quadsOf(const test::FakeRenderDevice& device) {
    constexpr usize kQuadVertices = 6;
    std::vector<Quad> quads;
    for (const test::RecordedDraw& draw : device.draws) {
        for (usize at = 0; at + kQuadVertices <= draw.vertices.size(); at += kQuadVertices) {
            const ImmediateVertex& first = draw.vertices[at];
            const ImmediateVertex& last = draw.vertices[at + 2];
            quads.push_back(
                {.texture = draw.texture,
                 .area = {first.position.x, first.position.y, last.position.x - first.position.x,
                          last.position.y - first.position.y},
                 .uv = {first.uv.x, first.uv.y, last.uv.x - first.uv.x, last.uv.y - first.uv.y},
                 .color = first.color,
                 .transform = draw.transform});
        }
    }
    return quads;
}

/** The panel slot a quad stands in, by its left edge. */
usize slotOf(const Quad& quad) {
    usize slot = 0;
    while (slot + 1 < Layout::kSlotX.size() &&
           quad.area.x >= static_cast<f32>(Layout::kSlotX[slot + 1])) {
        ++slot;
    }
    return slot;
}

/** The quads of picture `name` standing in `slot`, in turn; any slot's when none is named. */
std::vector<Quad> picturesIn(std::span<const Quad> quads, std::string_view name,
                             std::optional<usize> slot = std::nullopt) {
    std::vector<Quad> found;
    for (const Quad& quad : quads) {
        if (Fixture::pictureOf(quad.texture) == name && (!slot || slotOf(quad) == *slot)) {
            found.push_back(quad);
        }
    }
    return found;
}

/** The names of the pictures standing in `slot`, in the order drawn. */
std::vector<std::string_view> pictureOrder(std::span<const Quad> quads, usize slot) {
    std::vector<std::string_view> names;
    for (const Quad& quad : quads) {
        const std::string_view name = Fixture::pictureOf(quad.texture);
        if (!name.empty() && slotOf(quad) == slot) {
            names.push_back(name);
        }
    }
    return names;
}

/** Whether a quad is a glyph of one of the fixture's fonts. */
bool isText(const Quad& quad) {
    return Fixture::sheetOf(quad.texture, Fixture::kSmallFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kNameFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kNumberFont);
}

/** The area `quads` cover together. */
Rect covered(std::span<const Quad> quads) {
    REQUIRE_FALSE(quads.empty());
    f32 left = quads.front().area.x;
    f32 top = quads.front().area.y;
    f32 right = quads.front().area.right();
    f32 bottom = quads.front().area.bottom();
    for (const Quad& quad : quads) {
        left = std::min(left, quad.area.x);
        top = std::min(top, quad.area.y);
        right = std::max(right, quad.area.right());
        bottom = std::max(bottom, quad.area.bottom());
    }
    return {left, top, right - left, bottom - top};
}

/** Checks `actual` lies on `expected`, but for float rounding. */
void checkRect(const Rect& actual, const Rect& expected) {
    constexpr f64 kRounding = 1e-4;
    CHECK(actual.x == Approx(expected.x).margin(kRounding));
    CHECK(actual.y == Approx(expected.y).margin(kRounding));
    CHECK(actual.width == Approx(expected.width).margin(kRounding));
    CHECK(actual.height == Approx(expected.height).margin(kRounding));
}

/** A line one of the fixture's fonts wrote: its first glyph's place and its characters, read
 * back from where the glyphs were cut from the sheet, a space where the pen skipped one. */
struct Line {
    f32 x = 0;
    f32 y = 0;
    std::string text;
    bool operator==(const Line&) const = default;
};

/** The lines `font` wrote, in turn: each a run of its glyphs, one after another, along a top. */
std::vector<Line> linesOf(std::span<const Quad> quads, const test::N64Font& font) {
    std::vector<Line> lines;
    bool continuing = false;
    f32 previous = 0;
    for (const Quad& quad : quads) {
        if (!Fixture::sheetOf(quad.texture, font)) {
            continuing = false;
            continue;
        }
        // TextPainter samples a glyph from half a texel inside its cell.
        const auto cell = static_cast<usize>(
            std::lround((quad.uv.x * static_cast<f32>(font.sheetWidth()) - 0.5f) /
                        static_cast<f32>(font.columns)));
        const char code = font.codes.at(cell);
        const f32 gap = quad.area.x - previous;
        const auto advance = static_cast<f32>(font.columns);
        if (continuing && !lines.empty() && lines.back().y == quad.area.y &&
            (gap == advance || gap == advance + static_cast<f32>(font.space))) {
            if (gap != advance) {
                lines.back().text.push_back(' ');
            }
        } else {
            lines.push_back({quad.area.x, quad.area.y, {}});
        }
        lines.back().text.push_back(code);
        previous = quad.area.x;
        continuing = true;
    }
    return lines;
}

/** Where a glyph written from pen point (x, y) of panel `slot` lands: half a texel in. */
Line lineAt(usize slot, f32 x, f32 y, std::string text) {
    constexpr f32 kInset = 0.5f;
    return {static_cast<f32>(Layout::kSlotX.at(slot)) + x + kInset, kTop + y + kInset,
            std::move(text)};
}

/** A standing player's seat: class, costume colour and name, `health` of kMostHealth, the turbo
 * meter showing nothing. */
HudSeat standing(s32 classIndex, s32 color, std::string name, s32 health = kMostHealth) {
    HudSeat seat;
    seat.box.mode = StatusBoxView::Mode::Status;
    seat.box.active = true;
    seat.box.classIndex = classIndex;
    seat.box.color = color;
    seat.box.name = std::move(name);
    seat.box.health = health;
    seat.mostHealth = kMostHealth;
    seat.turbo = 0.0f;
    return seat;
}

/** `seat` fallen, as PartyHud::status shows it: health gone and nothing carried; in the tower,
 * asked to wait there or quit when `prompt`, or else still dying. */
HudSeat fallen(HudSeat seat, bool inTower, bool prompt = false, bool quitOffered = true) {
    seat.box.health = 0;
    seat.box.keys = 0;
    seat.box.potions = 0;
    seat.box.potionKind = 0;
    seat.box.inTower = inTower;
    seat.box.towerPrompt = inTower && prompt;
    seat.box.quitOffered = quitOffered;
    seat.turbo.reset();
    return seat;
}

/** The N64 look loaded from a synthetic extraction for `party`, with the device it uploads to,
 * Dark Legacy's art it may borrow and the words its panels write. */
struct LoadedLook {
    test::LegendsN64HudFixture files;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    LegendsN64HudPainter look;

    LoadedLook(std::string_view name, std::span<const HudMember> party) : files(name) {
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        REQUIRE(look.load(device, files.root(), art, party));
    }

    /** A view of `seats`, with these words. */
    HudView viewOf(std::array<HudSeat, HudView::kSeats> seats) const {
        HudView view;
        view.seats = std::move(seats);
        view.strings = &strings;
        return view;
    }

    /** The quads the look's panels draw for `view`, on a canvas of their own. */
    std::vector<Quad> panels(const HudView& view) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        look.drawStatus(frame, view);
        // The frame goes with the call: what the scene draws next is in canvas units again.
        CHECK(canvas.localTransform() == Mat4{1.0f});
        canvas.end();
        return quadsOf(device);
    }
};

/** A party of a warrior and a wizard in the first two seats. */
constexpr std::array<HudMember, 2> kWarriorAndWizard{{
    {.seat = 0, .classIndex = kWarriorClass, .color = 0},
    {.seat = 1, .classIndex = kWizardClass, .color = 1},
}};

} // namespace

TEST_CASE("the N64 look keeps the companion arrows above its panels", "[legends][n64-hud]") {
    LegendsN64HudPainter look;
    CHECK(look.id() == kLegendsN64HudStyle);
    CHECK(look.id() != kDarkLegacyHudStyle);
    // Frame row 170, the panels' top, on the canvas.
    CHECK(Layout::kStripTop == 170);
    CHECK(look.statusTop() == Approx(300.28).margin(0.005));
    CHECK(look.statusTop() == LegendsN64Frame::toCanvas({0, kTop}).y);

    // A look that never loaded draws nothing.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    HudView view;
    view.seats[0] = standing(kWarriorClass, 0, "ROWENA");
    view.strings = &strings;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    HudFrame frame{canvas, device, art};
    look.drawStatus(frame, view);
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("N64 panels stand in four slots along the bottom whatever the party size",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-slots", kWarriorAndWizard);
    const HudView four =
        n64.viewOf({standing(kWarriorClass, 0, "A"), standing(kWizardClass, 1, "B"),
                    standing(kWarriorClass, 2, "C"), standing(kWizardClass, 3, "D")});
    const HudView one = n64.viewOf({standing(kWarriorClass, 0, "A"), {}, {}, {}});
    for (const bool everyone : {true, false}) {
        CAPTURE(everyone);
        const std::vector<Quad> quads = n64.panels(everyone ? four : one);
        for (usize seat = 0; seat < Layout::kSlotX.size(); ++seat) {
            CAPTURE(seat);
            const Rect slot{static_cast<f32>(Layout::kSlotX[seat]), kTop,
                            static_cast<f32>(Layout::kPanelWidth),
                            static_cast<f32>(Layout::kPanelHeight)};
            // Every panel has its frame, two mirrored halves across the slot, whoever plays.
            const auto border = picturesIn(quads, "BORDER", seat);
            REQUIRE(border.size() == 2);
            checkRect(covered(border), slot);
            const bool played = everyone || seat == 0;
            if (played) {
                // COLOR's four quarters fill the slot.
                const auto background = picturesIn(quads, "COLOR", seat);
                REQUIRE(background.size() == 4);
                checkRect(covered(background), slot);
                CHECK(picturesIn(quads, "COLORI", seat).empty());
            } else {
                const auto background = picturesIn(quads, "COLORI", seat);
                REQUIRE(background.size() == 1);
                checkRect(background.front().area, slot);
                CHECK(picturesIn(quads, "COLOR", seat).empty());
            }
        }
        // All of it is drawn in the N64's frame, through its mapping onto the canvas.
        REQUIRE_FALSE(quads.empty());
        for (const Quad& quad : quads) {
            CHECK(quad.transform == LegendsN64Frame::transform());
        }
    }

    // COLOR is mirrored about its first texel on both axes, BORDER and TURBO across: the
    // texture's first column meets itself at the panel's middle.
    const std::vector<Quad> quads = n64.panels(one);
    const auto background = picturesIn(quads, "COLOR", 0);
    REQUIRE(background.size() == 4);
    checkRect(background[0].area, {0, kTop, 52, 25});
    checkRect(background[0].uv, {1, 1, -1, -1});
    checkRect(background[1].area, {52, kTop, 52, 25});
    checkRect(background[1].uv, {0, 1, 1, -1});
    checkRect(background[2].area, {0, kTop + 25, 52, 25});
    checkRect(background[2].uv, {1, 0, -1, 1});
    checkRect(background[3].area, {52, kTop + 25, 52, 25});
    checkRect(background[3].uv, {0, 0, 1, 1});
    for (const std::string_view name : {"BORDER", "TURBO"}) {
        CAPTURE(name);
        const auto halves = picturesIn(quads, name, 0);
        REQUIRE(halves.size() == 2);
        checkRect(halves[0].uv, {1, 0, -1, 1});
        checkRect(halves[1].uv, {0, 0, 1, 1});
    }
    checkRect(covered(picturesIn(quads, "TURBO", 0)), {0, kTop, 104, 15});
}

TEST_CASE("an N64 panel takes its costume's bright colour, an empty seat its own dark colour",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-colours", kWarriorAndWizard);
    // Seat 0 wears blue and seat 2 yellow; seats 1 and 3 are nobody's.
    const std::vector<Quad> quads = n64.panels(
        n64.viewOf({standing(kWarriorClass, 1, "A"), {}, standing(kWizardClass, 0, "B"), {}}));
    // e005a2b0's bright shades light a player's panel; e005a2d0's dark ones an empty one.
    const auto lit = [&](usize seat) {
        const auto background = picturesIn(quads, "COLOR", seat);
        REQUIRE(background.size() == 4);
        for (const Quad& quarter : background) {
            CHECK(quarter.color == background.front().color);
        }
        return background.front().color;
    };
    const auto empty = [&](usize seat) {
        const auto background = picturesIn(quads, "COLORI", seat);
        REQUIRE(background.size() == 1);
        return background.front().color;
    };
    CHECK(lit(0) == Color::rgba(0, 0, 220));
    CHECK(empty(1) == Color::rgba(0, 0, 110));
    CHECK(lit(2) == Color::rgba(219, 203, 5));
    CHECK(empty(3) == Color::rgba(0, 100, 12));
    CHECK(Layout::kBright[2] == Color::rgba(180, 0, 0));
    CHECK(Layout::kBright[3] == Color::rgba(0, 200, 25));
    CHECK(Layout::kDark[0] == Color::rgba(110, 102, 0));
    CHECK(Layout::kDark[2] == Color::rgba(110, 0, 0));
    // The frame keeps its own colours.
    for (const Quad& half : picturesIn(quads, "BORDER")) {
        CHECK(half.color == Color::white());
    }

    // COLOR and COLORI are I8, which decodes its intensity as alpha too: the N64 draws them
    // opaque, so their texels are.
    for (const std::string_view name : {"COLOR", "COLORI"}) {
        CAPTURE(name);
        const auto drawn = picturesIn(quads, name);
        REQUIRE_FALSE(drawn.empty());
        const auto& pixels = dynamic_cast<const test::FakeTexture&>(*drawn.front().texture).pixels;
        REQUIRE_FALSE(pixels.empty());
        for (usize alpha = 3; alpha < pixels.size(); alpha += 4) {
            REQUIRE(pixels[alpha] == 255);
        }
    }
}

TEST_CASE("the N64 turbo thirds brighten as the meter fills", "[legends][n64-hud]") {
    // FUN_e0059b44: each third rises from 50 as it fills and is lit whole, 255, once full.
    using Levels = std::array<s32, 3>;
    CHECK(legendsN64TurboLevels(0) == Levels{50, 50, 50});
    CHECK(legendsN64TurboLevels(33) == Levels{150, 50, 50});
    CHECK(legendsN64TurboLevels(33.9f) == Levels{150, 50, 50}); // what shows, whole
    CHECK(legendsN64TurboLevels(34) == Levels{255, 53, 50});
    CHECK(legendsN64TurboLevels(66) == Levels{255, 150, 50});
    CHECK(legendsN64TurboLevels(67) == Levels{255, 255, 52});
    CHECK(legendsN64TurboLevels(99) == Levels{255, 255, 147});
    CHECK(legendsN64TurboLevels(100) == Levels{255, 255, 255});
    CHECK(legendsN64TurboLevels(120) == Levels{255, 255, 255});
    CHECK(legendsN64TurboLevels(-5) == Levels{50, 50, 50});

    // A tile mirrored about its first texel, as FUN_e002b184 sets one up: below the texture's
    // width it runs backwards from the last texel, from there forwards from the first.
    const auto whole = legendsN64MirroredSpan(0, 104, 0, 52);
    REQUIRE(whole[0]);
    REQUIRE(whole[1]);
    CHECK(whole[0]->start == 0);
    CHECK(whole[0]->length == 52);
    CHECK(whole[0]->from == 1);
    CHECK(whole[0]->to == 0);
    CHECK(whole[1]->start == 52);
    CHECK(whole[1]->length == 52);
    CHECK(whole[1]->from == 0);
    CHECK(whole[1]->to == 1);
    // Red's outer light starts on the tile's first column one in from the edge, so the mask's
    // outer column, the texture's last, meets the frame's outer end.
    const auto outer = legendsN64MirroredSpan(1, 14, 0, 52);
    REQUIRE(outer[0]);
    CHECK_FALSE(outer[1]);
    CHECK(outer[0]->start == 1);
    CHECK(outer[0]->length == 14);
    CHECK(outer[0]->from == 1);
    CHECK(outer[0]->to == Approx(38.0 / 52));

    LoadedLook n64("n64-hud-turbo", kWarriorAndWizard);
    HudSeat player = standing(kWarriorClass, 0, "A");
    player.turbo = 50.0f;
    const std::vector<Quad> quads = n64.panels(n64.viewOf({HudSeat{}, player, {}, {}}));
    const auto lights = picturesIn(quads, "TURBOCLR");
    // Green across the middle, in two mirrored parts; yellow and red a part either side.
    REQUIRE(lights.size() == 6);
    const f32 left = static_cast<f32>(Layout::kSlotX[1]);
    struct Light {
        Rect area;
        f32 from = 0;
        f32 to = 0;
        Color color;
    };
    // At 50 green is lit whole, yellow at 101 and red at 50: (0x33,0xfd,0x08), (0xf5,0xff,0x16)
    // and (0xff,0x12,0x0a), each times its level over 255.
    const Color green = Color::rgba(0x33, 0xfd, 0x08);
    const Color yellow = Color::rgba(97, 101, 9);
    const Color red = Color::rgba(50, 4, 2);
    const std::array<Light, 6> expected{{
        {{left + 38, kTop, 14, 15}, 14.0f / 52, 0, green},
        {{left + 52, kTop, 14, 15}, 0, 14.0f / 52, green},
        {{left + 15, kTop, 22, 15}, 37.0f / 52, 15.0f / 52, yellow},
        {{left + 67, kTop, 21, 15}, 15.0f / 52, 36.0f / 52, yellow},
        {{left + 1, kTop, 14, 15}, 1, 38.0f / 52, red},
        {{left + 89, kTop, 14, 15}, 37.0f / 52, 51.0f / 52, red},
    }};
    for (usize index = 0; index < expected.size(); ++index) {
        CAPTURE(index);
        checkRect(lights[index].area, expected[index].area);
        checkRect(lights[index].uv,
                  {expected[index].from, 0, expected[index].to - expected[index].from, 1});
        CHECK(lights[index].color == expected[index].color);
    }
    // The lights come after every panel's words.
    std::optional<usize> lastWord;
    std::optional<usize> firstLight;
    for (usize index = 0; index < quads.size(); ++index) {
        if (isText(quads[index])) {
            lastWord = index;
        }
        if (!firstLight && Fixture::pictureOf(quads[index].texture) == "TURBOCLR") {
            firstLight = index;
        }
    }
    REQUIRE(lastWord);
    REQUIRE(firstLight);
    CHECK(*lastWord < *firstLight);
}

TEST_CASE("health, keys and potions sit where the N64 puts them, and only while carried",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-carried", kWarriorAndWizard);
    // Seat 0 is hurt, carries three keys and two of the blue potion; seat 1 is whole and carries
    // nothing; seat 2 stands past its level's most, as Dark Legacy's health may, with health
    // past what four digits hold, and a green potion.
    HudSeat hurt = standing(kWarriorClass, 0, "A", 250);
    hurt.box.keys = 3;
    hurt.box.potions = 2;
    hurt.box.potionKind = 2;
    const HudSeat whole = standing(kWizardClass, 1, "B");
    HudSeat over = standing(kWarriorClass, 2, "C", 12345);
    over.box.potions = 1;
    over.box.potionKind = 4;
    const std::vector<Quad> quads = n64.panels(n64.viewOf({hurt, whole, over, {}}));

    // HEART1 is centred on (36,13): opaque at full health, at transparency 0x78 short of it.
    const auto heart = [&](usize seat) {
        const auto drawn = picturesIn(quads, "HEART1", seat);
        REQUIRE(drawn.size() == 1);
        checkRect(drawn.front().area,
                  {static_cast<f32>(Layout::kSlotX[seat]) + 36 - 5.5f, kTop + 13 - 5, 11, 10});
        return drawn.front().color;
    };
    CHECK(heart(0) == Color::white().withAlpha(135));
    CHECK(heart(1) == Color::white());
    CHECK(heart(2) == Color::white());

    // Health is "%4d" in numfont from (41,9), held to four digits.
    const auto health = linesOf(quads, Fixture::kNumberFont);
    const f32 space = Fixture::kNumberFont.space;
    CHECK(health == std::vector<Line>{lineAt(0, 41 + space, 9, "250"),
                                      lineAt(1, 41 + space, 9, "600"), lineAt(2, 41, 9, "9999")});

    // KEY1 centred on (13,16) and its count from (20,10), only for seat 0; the bottle of the
    // potion thrown next centred on (89,16) and the count of all of them from (78,10).
    const auto keys = picturesIn(quads, "KEY1");
    REQUIRE(keys.size() == 1);
    CHECK(slotOf(keys.front()) == 0);
    checkRect(keys.front().area, {13 - 6, kTop + 16 - 7.5f, 12, 15});
    CHECK(keys.front().color == Color::white());
    const auto blue = picturesIn(quads, "POTIONB");
    REQUIRE(blue.size() == 1);
    CHECK(slotOf(blue.front()) == 0);
    checkRect(blue.front().area, {89 - 5.5f, kTop + 16 - 7, 11, 14});
    const auto green = picturesIn(quads, "POTIONG");
    REQUIRE(green.size() == 1);
    CHECK(slotOf(green.front()) == 2);
    // Kind 0 is an empty stack's, which would show red: nobody's bottle is red here.
    CHECK(picturesIn(quads, "POTIONR").empty());
    CHECK(picturesIn(quads, "POTIONY").empty());
    std::vector<Line> counts;
    for (const Line& line : linesOf(quads, Fixture::kSmallFont)) {
        if (line.y == kTop + 10.5f) {
            counts.push_back(line);
        }
    }
    CHECK(counts == std::vector<Line>{lineAt(0, 20, 10, "3"), lineAt(0, 78, 10, "2"),
                                      lineAt(2, 78, 10, "1")});
}

TEST_CASE("the arrows stand dim on the normal page", "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-arrows", kWarriorAndWizard);
    const std::vector<Quad> quads =
        n64.panels(n64.viewOf({standing(kWarriorClass, 0, "A"), {}, {}, {}}));
    // ARROW centred on (12,44), and turned about centred on (92,44), both at transparency 0x80
    // while no step lights one.
    const auto arrows = picturesIn(quads, "ARROW");
    REQUIRE(arrows.size() == 2);
    checkRect(arrows[0].area, {12 - 2, kTop + 44 - 3.5f, 4, 7});
    checkRect(arrows[0].uv, {0, 0, 1, 1});
    checkRect(arrows[1].area, {92 - 2, kTop + 44 - 3.5f, 4, 7});
    checkRect(arrows[1].uv, {1, 0, -1, 1});
    for (const Quad& arrow : arrows) {
        CHECK(arrow.color == Color::white().withAlpha(127));
    }
}

TEST_CASE("a class without an N64 portrait leaves the panel background, and its class line is "
          "Dark Legacy's",
          "[legends][n64-hud]") {
    const std::array<HudMember, 3> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kDwarfClass, .color = 1},
        {.seat = 2, .classIndex = kWizardClass, .color = 2},
    }};
    LoadedLook n64("n64-hud-classes", party);
    const std::vector<Quad> quads =
        n64.panels(n64.viewOf({standing(kWarriorClass, 0, "ABCDEFGHIJKL"),
                               standing(kDwarfClass, 1, "B"),
                               standing(kWizardClass, 2, "C"),
                               {}}));

    // The warrior's P1 is lit in the costume's bright shade, its P2 shines white at 0x7f over
    // it, both from (4,10), between the background and the frame.
    using Names = std::vector<std::string_view>;
    CHECK(pictureOrder(quads, 0) == Names{"COLOR", "COLOR", "COLOR", "COLOR", "WAR_P1", "WAR_P2",
                                          "BORDER", "BORDER", "TURBO", "TURBO", "HEART1", "ARROW",
                                          "ARROW", "TURBOCLR", "TURBOCLR", "TURBOCLR", "TURBOCLR",
                                          "TURBOCLR", "TURBOCLR"});
    const auto light = picturesIn(quads, "WAR_P1", 0);
    REQUIRE(light.size() == 1);
    checkRect(light.front().area, {4, kTop + 10, 35, 33});
    checkRect(light.front().uv, {0, 0, 1, 1});
    CHECK(light.front().color == Layout::kBright[0]);
    const auto shine = picturesIn(quads, "WAR_P2", 0);
    REQUIRE(shine.size() == 1);
    checkRect(shine.front().area, {4, kTop + 10, 35, 33});
    CHECK(shine.front().color == Color::white().withAlpha(0x7f));

    // The N64 has no dwarf: the panel's background shows where the portrait would be.
    CHECK(pictureOrder(quads, 1) == Names{"COLOR", "COLOR", "COLOR", "COLOR", "BORDER", "BORDER",
                                          "TURBO", "TURBO", "HEART1", "ARROW", "ARROW", "TURBOCLR",
                                          "TURBOCLR", "TURBOCLR", "TURBOCLR", "TURBOCLR",
                                          "TURBOCLR"});

    // WIZ_P1 is taller than the others: it is clipped to their 35 by 33.
    const auto tall = picturesIn(quads, "WIZ_P1", 2);
    REQUIRE(tall.size() == 1);
    checkRect(tall.front().area, {208 + 4, kTop + 10, 35, 33});
    checkRect(tall.front().uv, {0, 0, 1, 33.0f / 35});
    CHECK(tall.front().color == Layout::kBright[2]);

    // The class line is the N64's word for its classes (N64 strings 129-138) and Dark Legacy's
    // lower-case name for the rest, from (20,40) in font 2, which writes them in capitals; the
    // name is cut to ten characters, from (25,30) in glfm3.
    CHECK(linesOf(quads, Fixture::kSmallFont) == std::vector<Line>{lineAt(0, 20, 40, "WARRIOR"),
                                                                   lineAt(1, 20, 40, "DWARF"),
                                                                   lineAt(2, 20, 40, "WIZARD")});
    CHECK(linesOf(quads, Fixture::kNameFont) == std::vector<Line>{lineAt(0, 25, 30, "ABCDEFGHIJ"),
                                                                  lineAt(1, 25, 30, "B"),
                                                                  lineAt(2, 25, 30, "C")});
}

TEST_CASE("a character carrying Pojo is named Pojo on its N64 panel", "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-pojo", kWarriorAndWizard);
    CharacterSave save;
    save.name = "ROWENA";
    save.character = kWarriorClass;
    save.color = 0;
    save.progress().inventory.permanentPojo = true;
    PlayerRuntime runtime;
    runtime.actor.spawn(0, save, nullptr, Vec3{0}, 0);
    HudSeat carrier = standing(kWarriorClass, 0, "ROWENA");
    carrier.runtime = &runtime;
    const std::vector<Quad> quads = n64.panels(n64.viewOf({carrier, {}, {}, {}}));
    // The N64's word for its class 4, as Dark Legacy's messages name a character carrying him;
    // the portrait stays the warrior's.
    CHECK(linesOf(quads, Fixture::kSmallFont) == std::vector<Line>{lineAt(0, 20, 40, "POJO")});
    CHECK(picturesIn(quads, "WAR_P1", 0).size() == 1);
}

TEST_CASE("a fallen player's N64 panel is the empty panel, with Dark Legacy's prompt when it has "
          "one",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-fallen", kWarriorAndWizard);
    // Seat 0 is asked to wait in the tower or quit; seat 1 the same online, where it may not
    // quit; seat 2 waits in the tower; seat 3 is still dying.
    const HudSeat asked = fallen(standing(kWarriorClass, 2, "A"), true, true);
    const HudSeat online = fallen(standing(kWizardClass, 3, "B"), true, true, false);
    const HudSeat waiting = fallen(standing(kWarriorClass, 1, "C"), true);
    const HudSeat dying = fallen(standing(kWizardClass, 0, "D"), false);
    const std::vector<Quad> quads = n64.panels(n64.viewOf({asked, online, waiting, dying}));

    // In the tower the panel is COLORI in the costume's dark shade, under the frame, and nothing
    // else: no portrait, turbo meter, heart or arrows.
    using Names = std::vector<std::string_view>;
    for (usize seat = 0; seat < 3; ++seat) {
        CAPTURE(seat);
        CHECK(pictureOrder(quads, seat) == Names{"COLORI", "BORDER", "BORDER"});
    }
    CHECK(picturesIn(quads, "COLORI", 0).front().color == Layout::kDark[2]);
    CHECK(picturesIn(quads, "COLORI", 1).front().color == Layout::kDark[3]);
    CHECK(picturesIn(quads, "COLORI", 2).front().color == Layout::kDark[1]);

    // Dark Legacy's two choices are written in font 2, centred on the panel at tops 16 and 28;
    // the online one offers no quitting.
    const auto measure = [](std::string_view text) {
        s32 width = 0;
        for (const char code : text) {
            width += code == ' ' ? Fixture::kSmallFont.space : Fixture::kSmallFont.columns;
        }
        return width;
    };
    // TextPainter centres a line on its whole width, halved down.
    const auto centred = [&](usize slot, f32 top, std::string text) {
        const auto left = static_cast<f32>(Layout::kPromptCentre - measure(text) / 2);
        return lineAt(slot, left, top, std::move(text));
    };
    std::vector<Line> words;
    for (const Line& line : linesOf(quads, Fixture::kSmallFont)) {
        if (slotOf({.area = {line.x, line.y, 0, 0}}) != 3) {
            words.push_back(line);
        }
    }
    CHECK(words == std::vector<Line>{centred(0, 16, "WAIT IN TOWER"), centred(0, 28, "QUIT GAME"),
                                     centred(1, 16, "WAIT IN TOWER")});
    CHECK(Layout::kPromptLineTops == std::array<s32, 2>{16, 28});

    // Dying, the panel is the whole normal one, with nothing carried, health 0 and the name.
    CHECK(pictureOrder(quads, 3) == Names{"COLOR", "COLOR", "COLOR", "COLOR", "WIZ_P1", "WIZ_P2",
                                          "BORDER", "BORDER", "TURBO", "TURBO", "HEART1", "ARROW",
                                          "ARROW"});
    CHECK(linesOf(quads, Fixture::kNumberFont) ==
          std::vector<Line>{lineAt(3, 41 + 3 * f32{Fixture::kNumberFont.space}, 9, "0")});
    CHECK(linesOf(quads, Fixture::kNameFont) == std::vector<Line>{lineAt(3, 25, 30, "D")});
    // No turbo lights for anyone fallen, and none of it in Dark Legacy's own small capitals.
    CHECK(picturesIn(quads, "TURBOCLR").empty());
    for (const Quad& quad : quads) {
        CHECK(quad.texture != &n64.art.smallCapsSheet());
    }
}

TEST_CASE("a missing class bank never stops the N64 look, a missing or truncated HUD bank or "
          "font does, and a truncated class bank falls back",
          "[legends][n64-hud]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto portraitsOf = [&](LegendsN64HudPainter& look) {
        test::FakeRenderDevice drawing;
        Canvas canvas;
        canvas.begin(drawing, Mat4{1.0f});
        HudFrame frame{canvas, drawing, art};
        HudView view;
        view.seats[0] = standing(kWarriorClass, 0, "A");
        view.seats[1] = standing(kWizardClass, 1, "B");
        view.strings = &strings;
        look.drawStatus(frame, view);
        canvas.end();
        const std::vector<Quad> quads = quadsOf(drawing);
        // The panels stand either way.
        CHECK(picturesIn(quads, "COLOR").size() == 8);
        std::vector<std::string_view> names;
        for (const std::string_view name : {"WAR_P1", "WAR_P2", "WIZ_P1", "WIZ_P2"}) {
            if (!picturesIn(quads, name).empty()) {
                names.push_back(name);
            }
        }
        return names;
    };
    using Names = std::vector<std::string_view>;

    SECTION("everything there") {
        const Fixture files("n64-hud-load-whole");
        LegendsN64HudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
        CHECK(portraitsOf(look) == Names{"WAR_P1", "WAR_P2", "WIZ_P1", "WIZ_P2"});
    }
    SECTION("a class bank missing") {
        const Fixture files("n64-hud-load-no-warrior");
        std::filesystem::remove(files.root() / Fixture::kWarriorBank);
        LegendsN64HudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), art, kWarriorAndWizard); });
        CHECK(loaded);
        CHECK(run.warnings.empty());
        CHECK(portraitsOf(look) == Names{"WIZ_P1", "WIZ_P2"});
    }
    SECTION("a class bank cut short") {
        const Fixture files("n64-hud-load-short-warrior");
        files.truncate(Fixture::kWarriorBank);
        LegendsN64HudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), art, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == 1);
        CHECK(run.warnings.front().find("hud/warf/objects.wad") != std::string::npos);
        // The warrior's portrait area shows the panel's background.
        CHECK(portraitsOf(look) == Names{"WIZ_P1", "WIZ_P2"});
    }
    SECTION("the HUD bank or a font missing") {
        for (const std::string_view name :
             {LegendsN64HudPainter::kHudBank, LegendsN64HudPainter::kSmallFontFile,
              LegendsN64HudPainter::kNameFontFile, LegendsN64HudPainter::kNumberFontFile}) {
            CAPTURE(name);
            const Fixture files("n64-hud-load-missing");
            std::filesystem::remove(files.root() / name);
            LegendsN64HudPainter look;
            CHECK_FALSE(look.load(device, files.root(), art, kWarriorAndWizard));
        }
    }
    SECTION("the HUD bank or a font cut short") {
        // What cannot be read stops the look with its cause, for the opener's one warning.
        for (const std::string_view name :
             {LegendsN64HudPainter::kHudBank, LegendsN64HudPainter::kSmallFontFile,
              LegendsN64HudPainter::kNameFontFile, LegendsN64HudPainter::kNumberFontFile}) {
            CAPTURE(name);
            const Fixture files("n64-hud-load-short");
            files.truncate(name);
            LegendsN64HudPainter look;
            try {
                (void)look.load(device, files.root(), art, kWarriorAndWizard);
                FAIL("a cut-short file was read");
            } catch (const FormatError& error) {
                CHECK(std::string_view{error.what()}.starts_with(name));
            }
        }
    }
    SECTION("a HUD bank without a picture the panels draw") {
        for (const test::N64Picture& left : Fixture::kPanelPictures) {
            CAPTURE(left.name);
            const Fixture files("n64-hud-load-incomplete");
            std::vector<test::N64Picture> pictures;
            for (const test::N64Picture& picture : Fixture::kPanelPictures) {
                if (picture.name != left.name) {
                    pictures.push_back(picture);
                }
            }
            files.writeBank(LegendsN64HudPainter::kHudBank, pictures);
            LegendsN64HudPainter look;
            CHECK_THROWS_AS(look.load(device, files.root(), art, kWarriorAndWizard), FormatError);
        }
    }
}

TEST_CASE("the N64 look takes every picture it borrows as it opens", "[legends][n64-hud]") {
    test::LegendsN64HudFixture files("n64-hud-borrowed");
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    // Dark Legacy's art stands ready to lend what a look might ask of it.
    std::vector<const Texture*> lent;
    for (const auto set :
         {test::DarkLegacyLookup::Set::Static, test::DarkLegacyLookup::Set::Select,
          test::DarkLegacyLookup::Set::Count, test::DarkLegacyLookup::Set::Inventory}) {
        for (const std::string_view name : {"RUNE13", "SM_KEY_BLU", "S4_WAR", "SHP_XRAY_GLASSES"}) {
            lent.push_back(&art.give(set, name));
        }
    }
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LegendsN64HudPainter look;
    REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
    // The panels draw only the N64's own art: they borrow nothing of Dark Legacy's.
    CHECK(art.lookups.empty());
    const u32 uploaded = device.texturesCreated;
    REQUIRE(uploaded > 0);

    // Every seat state and every part of the HUD, drawn twice: nothing is looked up or
    // uploaded while drawing.
    HudSeat carrying = standing(kWarriorClass, 0, "A", 250);
    carrying.box.keys = 2;
    carrying.box.potions = 1;
    carrying.box.potionKind = 3;
    HudView view;
    view.seats = {carrying,
                  fallen(standing(kWizardClass, 1, "B"), true, true),
                  fallen(standing(kWizardClass, 2, "C"), false),
                  {}};
    view.strings = &strings;
    for (s32 frame = 0; frame < 2; ++frame) {
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame hud{canvas, device, art};
        look.prepare(hud, view);
        look.drawStatus(hud, view);
        look.drawRuneMeter(hud, view);
        look.drawHourglass(hud, view);
        look.drawBossMeters(hud, view);
        look.drawSelectors(hud, view);
        look.drawHelp(hud, view);
        canvas.end();
    }
    CHECK(art.lookups.empty());
    CHECK(device.texturesCreated == uploaded);
    REQUIRE_FALSE(device.draws.empty());
    for (const test::RecordedDraw& draw : device.draws) {
        CHECK(std::ranges::find(lent, draw.texture) == lent.end());
    }
}

TEST_CASE("the N64 look draws the parts it has no art of its own for in Dark Legacy's look",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-lent-look", kWarriorAndWizard);
    const HudView view = n64.viewOf({standing(kWarriorClass, 0, "A"), {}, {}, {}});
    Canvas canvas;
    canvas.begin(n64.device, Mat4{1.0f});
    HudFrame frame{canvas, n64.device, n64.art};
    n64.look.prepare(frame, view);
    n64.look.drawStatus(frame, view);
    n64.look.drawRuneMeter(frame, view);
    n64.look.drawHourglass(frame, view);
    n64.look.drawBossMeters(frame, view);
    n64.look.drawSelectors(frame, view);
    n64.look.drawHelp(frame, view);
    canvas.end();
    // The runestone finder, the hourglasses, the boss meters, the selector labels and the help
    // are Dark Legacy's, prepared as its look prepares them; the panels are the N64's own.
    const test::FakeHudRecord& darkLegacy = n64.art.record;
    for (const test::FakeHudPart part :
         {test::FakeHudPart::Prepare, test::FakeHudPart::RuneMeter, test::FakeHudPart::Hourglass,
          test::FakeHudPart::BossMeters, test::FakeHudPart::Selectors, test::FakeHudPart::Help}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 1);
    }
    CHECK(darkLegacy.count(kDarkLegacyHudStyle, test::FakeHudPart::Status) == 0);
    CHECK(darkLegacy.calls.size() == 6);
}
