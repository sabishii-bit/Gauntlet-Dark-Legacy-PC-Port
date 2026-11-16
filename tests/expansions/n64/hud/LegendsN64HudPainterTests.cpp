#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64HudFixture.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "formats/AnimationTree.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerControls.h"

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
    Rect area{};
    Rect uv{};
    Color color{};
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
 * Dark Legacy's art it may borrow, lending the count icons `countIcons`, and the words its
 * panels write. */
struct LoadedLook {
    test::LegendsN64HudFixture files;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    LegendsN64HudPainter look;

    LoadedLook(std::string_view name, std::span<const HudMember> party,
               std::initializer_list<std::string_view> countIcons = {})
        : files(name) {
        for (const std::string_view icon : countIcons) {
            art.give(test::DarkLegacyLookup::Set::Count, icon);
        }
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

    using Slot = void (LegendsN64HudPainter::*)(HudFrame&, const HudView&);
    /** The quads the look's `slot` draws for `view`, on a canvas of its own. */
    std::vector<Quad> drawn(const HudView& view, Slot slot) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        (look.*slot)(frame, view);
        // The frame goes with the call: what the scene draws next is in canvas units again.
        CHECK(canvas.localTransform() == Mat4{1.0f});
        canvas.end();
        return quadsOf(device);
    }
    /** The quads the look's panels draw for `view`. */
    std::vector<Quad> panels(const HudView& view) {
        return drawn(view, &LegendsN64HudPainter::drawStatus);
    }
    /** Readies the look for a frame of `view`, as the scene does before its draws. */
    void prepare(const HudView& view) {
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        look.prepare(frame, view);
        canvas.end();
    }
};

/** The play clock at the middle of the look's step `step`. */
f32 secondsAt(s32 step) {
    return (static_cast<f32>(step) + 0.5f) / LegendsHudClock::kStepsPerSecond;
}

/** A boss's meter art: two strips, each a background and a fill `width` by `height`. */
TextureSet meterArt(u32 width, u32 height) {
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (const char* name : {"METER_BG1", "METER_FG1", "METER_BG2", "METER_FG2"}) {
        entries.push_back({.name = name, .width = width, .height = height, .file = {}});
        images.push_back(Image::filled(width, height, Color::white()));
    }
    return TextureSet::fromDecoded(std::move(entries), std::move(images));
}

/** A boss's meter of `pieces` strips with a background each and these insets, at `health` of
 * `maxHealth`. */
HealthMeterReading meterReading(s32 pieces, s32 leftInset, s32 rightInset, f32 health,
                                f32 maxHealth) {
    HealthMeterReading reading;
    reading.definition.pieces = pieces;
    reading.definition.advance = BossMeter::kPieceWidth;
    reading.definition.leftInset = leftInset;
    reading.definition.rightInset = rightInset;
    reading.definition.shown = true;
    reading.definition.backed = true;
    reading.health = health;
    reading.maxHealth = maxHealth;
    return reading;
}

/** The meters of `readings` drawn from `art`, as the scene binds them, and told to `view`. */
void showMeters(HudView& view, BossMeters& meters, std::span<const HealthMeterReading> readings,
                TextureSet& art) {
    meters.bind(readings, &art);
    view.bossMeters = &meters;
    view.bossCount = hudBossReadingsOf(readings, view.bossReadings);
}

/** The picture `name` of `art`, uploaded to `device` as a meter's draw uploads it. */
const Texture* meterPicture(TextureSet& art, test::FakeRenderDevice& device,
                            std::string_view name) {
    const auto index = art.find(name);
    REQUIRE(index.has_value());
    return &art.texture(device, *index);
}

/** Dark Legacy's hourglass art as plain pictures: the glass, its sand and two frames of
 * falling grains, which SAND_ANIM cycles. */
ItemArchive darkLegacyHourglass() {
    std::vector<TextureSetEntry> entries{
        {.name = "TIMER", .width = 128, .height = 128, .file = {}},
        {.name = "TIMER_SAND", .width = 128, .height = 128, .file = {}},
        {.name = "SAND_ANIM00", .width = 8, .height = 32, .file = {}},
        {.name = "SAND_ANIM01", .width = 8, .height = 32, .file = {}}};
    std::vector<Image> images;
    images.reserve(entries.size());
    for (const TextureSetEntry& entry : entries) {
        images.push_back(Image::filled(entry.width, entry.height, Color::white()));
    }
    ItemArchive archive;
    archive.textures = TextureSet::fromDecoded(std::move(entries), std::move(images));
    formats::AnimationFile animations;
    animations.textureAnimations.push_back(
        {.name = "SAND_ANIM",
         .frameName = {},
         .texture = 2,
         .source = 2,
         .frames = 2,
         .rate = 1,
         .flag = static_cast<s16>(TextureAnimationInfo::kFreeRunning)});
    archive.trees = AnimationSet::fromDecoded(std::move(animations));
    return archive;
}

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
    // On the normal page the bottle is opaque, as the key is.
    CHECK(blue.front().color == Color::white());
    const auto green = picturesIn(quads, "POTIONG");
    REQUIRE(green.size() == 1);
    CHECK(slotOf(green.front()) == 2);
    CHECK(green.front().color == Color::white());
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
        const s32 left = Layout::kPromptCentre - measure(text) / 2;
        return lineAt(slot, static_cast<f32>(left), top, std::move(text));
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
        for (const std::string_view name : LegendsN64HudPainter::kRequiredFiles) {
            CAPTURE(name);
            const Fixture files("n64-hud-load-missing");
            std::filesystem::remove(files.root() / name);
            LegendsN64HudPainter look;
            CHECK_FALSE(look.load(device, files.root(), art, kWarriorAndWizard));
        }
    }
    SECTION("the HUD bank or a font cut short") {
        // What cannot be read stops the look with its cause, for the opener's one warning.
        for (const std::string_view name : LegendsN64HudPainter::kRequiredFiles) {
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
    SECTION("a HUD bank without a picture the look draws") {
        // The panels' pictures and the advice scrolls alike.
        for (const test::N64Picture& left : Fixture::kHudPictures) {
            CAPTURE(left.name);
            const Fixture files("n64-hud-load-incomplete");
            std::vector<test::N64Picture> pictures;
            for (const test::N64Picture& picture : Fixture::kHudPictures) {
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
    // Dark Legacy's art stands ready to lend what a look might ask of it, and a crystal's
    // count icon.
    std::vector<const Texture*> lent;
    for (const auto set :
         {test::DarkLegacyLookup::Set::Static, test::DarkLegacyLookup::Set::Select,
          test::DarkLegacyLookup::Set::Count, test::DarkLegacyLookup::Set::Inventory}) {
        for (const std::string_view name : {"RUNE13", "SM_KEY_BLU", "S4_WAR", "SHP_XRAY_GLASSES"}) {
            lent.push_back(&art.give(set, name));
        }
    }
    const Texture& crystal =
        art.give(test::DarkLegacyLookup::Set::Count, PickupHud::crystalIcon(1));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LegendsN64HudPainter look;
    REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
    // As it opens it takes every icon Dark Legacy's pickup counts may show, and nothing else:
    // the panels, the hourglass, the help and the meters draw the N64's own art.
    std::vector<test::DarkLegacyLookup> borrowed;
    for (const std::string& icon : PickupHud::countIcons()) {
        borrowed.push_back({test::DarkLegacyLookup::Set::Count, icon});
    }
    CHECK(art.lookups == borrowed);

    // A boss meter, its pictures up as its archive keeps them once drawn.
    TextureSet meterPictures = meterArt(LegendsN64HudLayout::kMeterStripWidth, 32);
    const std::array<HealthMeterReading, 1> dragon{meterReading(2, 46, 72, 1875, 2500)};
    BossMeters meters;
    meters.bind(dragon, &meterPictures);
    meters.meter(0).preload(device);
    const u32 uploaded = device.texturesCreated;
    REQUIRE(uploaded > 0);

    // Every seat state and every part of the HUD, drawn twice: nothing is looked up or
    // uploaded while drawing.
    HudSeat carrying = standing(kWarriorClass, 0, "A", 250);
    carrying.box.keys = 2;
    carrying.box.potions = 1;
    carrying.box.potionKind = 3;
    PickupHud pickups;
    pickups.showCount(0, PickupHud::crystalIcon(1), 3, 15);
    const std::vector<std::string> lines{"WATCH OUT"};
    HudView view;
    view.seats = {carrying,
                  fallen(standing(kWizardClass, 1, "B"), true, true),
                  fallen(standing(kWizardClass, 2, "C"), false),
                  {}};
    view.strings = &strings;
    view.pickups = &pickups;
    view.help = HudHelp{.lines = lines, .player = 0, .color = 0, .head = {256, 300}};
    view.hourglass = ChallengeHud::Look{.elapsed = 0.5f, .fallingFrame = 2};
    view.bossMeters = &meters;
    view.bossCount = hudBossReadingsOf(dragon, view.bossReadings);
    view.levelHud = kLegendsN64HudStyle;
    for (s32 frame = 0; frame < 2; ++frame) {
        view.seconds = secondsAt(frame);
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
    CHECK(art.lookups == borrowed);
    CHECK(device.texturesCreated == uploaded);
    REQUIRE_FALSE(device.draws.empty());
    // It draws its own art, the meter's and the icon it took, and no other of Dark Legacy's.
    for (const test::RecordedDraw& draw : device.draws) {
        CHECK(std::ranges::find(lent, draw.texture) == lent.end());
    }
    CHECK(std::ranges::any_of(
        device.draws, [&](const test::RecordedDraw& draw) { return draw.texture == &crystal; }));
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
    // The runestone finder and the selector labels are Dark Legacy's, prepared as its look
    // prepares them; the panels, the hourglass, the boss meters and the help are the N64's own.
    const test::FakeHudRecord& darkLegacy = n64.art.record;
    for (const test::FakeHudPart part :
         {test::FakeHudPart::Prepare, test::FakeHudPart::RuneMeter, test::FakeHudPart::Selectors}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 1);
    }
    for (const test::FakeHudPart part : {test::FakeHudPart::Status, test::FakeHudPart::Hourglass,
                                         test::FakeHudPart::BossMeters, test::FakeHudPart::Help}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 0);
    }
    CHECK(darkLegacy.calls.size() == 3);
}

TEST_CASE("the N64 hourglass drains its top bulb and fills the bottom", "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-hourglass", kWarriorAndWizard);
    HudView view = n64.viewOf({});
    // No hourglass, nothing drawn.
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawHourglass).empty());

    // FUN_e0069ee0: TIMER_P0 at (15,25), and TIMER_P1 from the same corner, cropped to the rows
    // of sand the top bulb has left and the bottom one has gathered; SAND_ANIM's grains fall
    // from (54,56) to meet it. The glass and both sands at transparency 0x40, the grains opaque.
    const Color clouded = Color::white().withAlpha(191);
    struct Piece {
        std::string_view picture;
        Rect area;
        Rect uv;
        Color color;
    };
    const Piece glass{"TIMER_P0", {15, 25, 82, 64}, {0, 0, 1, 1}, clouded};
    const auto check = [&](const ChallengeHud::Look& look, const std::vector<Piece>& expected) {
        view.hourglass = look;
        const std::vector<Quad> quads = n64.drawn(view, &LegendsN64HudPainter::drawHourglass);
        REQUIRE(quads.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(Fixture::pictureOf(quads[index].texture) == expected[index].picture);
            checkRect(quads[index].area, expected[index].area);
            checkRect(quads[index].uv, expected[index].uv);
            CHECK(quads[index].color == expected[index].color);
            // In the frame, not a panel.
            CHECK(quads[index].transform == LegendsN64Frame::transform());
        }
    };
    // f = 0: the top bulb full and the grains falling their whole way; nothing below yet. The
    // grains' frame is the falling counter's, held to the five.
    check({.elapsed = 0, .fallingFrame = 7},
          {glass,
           {"TIMER_P1", {15, 36, 82, 20}, {0, 11.0f / 64, 1, 20.0f / 64}, clouded},
           {"SAND_ANIM02", {54, 56, 4, 20}, {0, 0, 1, 20.0f / 21}, Color::white()}});
    // f = 0.5: half of each bulb, the stream half as long.
    check({.elapsed = 0.5f, .fallingFrame = 3},
          {glass,
           {"TIMER_P1", {15, 46, 82, 10}, {0, 21.0f / 64, 1, 10.0f / 64}, clouded},
           {"TIMER_P1", {15, 66, 82, 10}, {0, 41.0f / 64, 1, 10.0f / 64}, clouded},
           {"SAND_ANIM03", {54, 56, 4, 10}, {0, 0, 1, 10.0f / 21}, Color::white()}});
    // f = 1: everything below, nothing left to fall.
    check({.elapsed = 1, .fallingFrame = 0},
          {glass, {"TIMER_P1", {15, 56, 82, 20}, {0, 31.0f / 64, 1, 20.0f / 64}, clouded}});
    // While the clock stands still the bottom bulb and the grains are hidden.
    check({.elapsed = 0.5f, .fallingFrame = -1},
          {glass, {"TIMER_P1", {15, 46, 82, 10}, {0, 21.0f / 64, 1, 10.0f / 64}, clouded}});
    // The N64's own art: Dark Legacy's look is not asked.
    CHECK(n64.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Hourglass) == 0);
}

TEST_CASE("without its timer art the N64 look draws Dark Legacy's hourglass",
          "[legends][n64-hud]") {
    // Dark Legacy's own look lends its art, and draws the hourglass it is given.
    test::FakeRenderDevice device;
    DarkLegacyHudPainter darkLegacy;
    ItemArchive archive = darkLegacyHourglass();
    ChallengeHud stopTime;
    REQUIRE(stopTime.bind(device, archive));
    HudView view;
    view.hourglass = stopTime.look(45, 60, true);
    view.hourglassArt = &stopTime;
    REQUIRE(view.hourglass->fallingFrame >= 0);
    const auto drawsOf = [&](HudPainter& look) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, darkLegacy};
        look.drawHourglass(frame, view);
        canvas.end();
        return device.draws;
    };
    const std::vector<test::RecordedDraw> expected = drawsOf(darkLegacy);
    REQUIRE(expected.size() == 3);
    const auto check = [&](const Fixture& files, usize warnings) {
        LegendsN64HudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), darkLegacy, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == warnings);
        for (const std::string& warning : run.warnings) {
            CHECK(warning.find(LegendsN64HudPainter::kTimerBank) != std::string::npos);
        }
        const std::vector<test::RecordedDraw> drawn = drawsOf(look);
        REQUIRE(drawn.size() == expected.size());
        for (usize index = 0; index < drawn.size(); ++index) {
            CAPTURE(index);
            test::checkRecordedDraw(drawn[index], expected[index]);
        }
        return run;
    };
    SECTION("no timer bank") {
        const Fixture files("n64-hud-no-timer");
        std::filesystem::remove(files.root() / LegendsN64HudPainter::kTimerBank);
        check(files, 0);
    }
    SECTION("a timer bank cut short") {
        const Fixture files("n64-hud-short-timer");
        files.truncate(LegendsN64HudPainter::kTimerBank);
        check(files, 1);
    }
    SECTION("a timer bank without a frame of the grains") {
        const Fixture files("n64-hud-incomplete-timer");
        std::vector<test::N64Picture> pictures;
        for (const test::N64Picture& picture : Fixture::kTimerPictures) {
            if (picture.name != "SAND_ANIM03") {
                pictures.push_back(picture);
            }
        }
        files.writeBank(LegendsN64HudPainter::kTimerBank, pictures);
        const auto run = check(files, 1);
        CHECK(run.warnings.front().find("SAND_ANIM03") != std::string::npos);
    }
}

TEST_CASE("N64 boss meters stand at the top, in the N64's frame for N64 bosses and in the canvas "
          "for others",
          "[legends][n64-hud]") {
    // FUN_e0888a7c: the first strip's fill from its cap to its end over the first half of the
    // health, the second's from its start to its tail over the rest; a 200-wide strip takes its
    // type's insets at 200/256, the dragon's 46 and 72 at 35.9375 and 56.25.
    using Widths = std::array<s32, 2>;
    constexpr f32 kCap = 35.9375f;
    constexpr f32 kTail = 56.25f;
    CHECK(legendsN64MeterWidths(0, 200, kCap, kTail) == Widths{35, 0});
    CHECK(legendsN64MeterWidths(0.5f, 200, kCap, kTail) == Widths{117, 0});
    CHECK(legendsN64MeterWidths(1, 200, kCap, kTail) == Widths{200, 0});
    CHECK(legendsN64MeterWidths(1.5f, 200, kCap, kTail) == Widths{200, 71});
    CHECK(legendsN64MeterWidths(2, 200, kCap, kTail) == Widths{200, 143});
    CHECK(legendsN64MeterWidths(3, 200, kCap, kTail) == Widths{200, 143});
    CHECK(legendsN64MeterWidths(-1, 200, kCap, kTail) == Widths{35, 0});

    LoadedLook n64("n64-hud-meters", kWarriorAndWizard);
    HudView view = n64.viewOf({});
    // No meter, nothing drawn.
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawBossMeters).empty());
    struct Piece {
        const Texture* texture = nullptr;
        Rect area;
        Rect uv;
    };
    const auto check = [&](const std::vector<Quad>& quads, const std::vector<Piece>& expected,
                           const Mat4& transform) {
        REQUIRE(quads.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(quads[index].texture == expected[index].texture);
            checkRect(quads[index].area, expected[index].area);
            checkRect(quads[index].uv, expected[index].uv);
            // Opaque and never tinted.
            CHECK(quads[index].color == Color::white());
            CHECK(quads[index].transform == transform);
        }
    };

    // An N64 boss in its own level: the dragon at three quarters, its art 200 by 32, in the frame
    // at (left + 200n, 1).
    TextureSet n64Art = meterArt(LegendsN64HudLayout::kMeterStripWidth, 32);
    const std::array<HealthMeterReading, 1> dragon{meterReading(2, 46, 72, 1875, 2500)};
    BossMeters n64Meters;
    showMeters(view, n64Meters, dragon, n64Art);
    view.levelHud = kLegendsN64HudStyle;
    n64.prepare(view);
    const std::vector<Quad> native = n64.drawn(view, &LegendsN64HudPainter::drawBossMeters);
    const auto n64Picture = [&](std::string_view name) {
        return meterPicture(n64Art, n64.device, name);
    };
    check(native,
          {{n64Picture("METER_BG1"), {0, 1, 200, 32}, {0, 0, 1, 1}},
           {n64Picture("METER_FG1"), {0, 1, 200, 32}, {0, 0, 1, 1}},
           {n64Picture("METER_BG2"), {200, 1, 200, 32}, {0, 0, 1, 1}},
           {n64Picture("METER_FG2"), {200, 1, 71, 32}, {0, 0, 71.0f / 200, 1}}},
          LegendsN64Frame::transform());

    // A Dark Legacy boss, its art drawn for the canvas: the lich at three quarters, in the
    // canvas at (left + 256n, 10), and left untinted while frozen.
    TextureSet darkArt = meterArt(BossMeter::kPieceWidth, 16);
    const std::array<HealthMeterReading, 1> lich{meterReading(2, 44, 53, 2250, 3000)};
    BossMeters darkMeters;
    showMeters(view, darkMeters, lich, darkArt);
    darkMeters.update(0, lich, true, true);
    view.levelHud = {};
    n64.prepare(view);
    const auto darkPicture = [&](std::string_view name) {
        return meterPicture(darkArt, n64.device, name);
    };
    check(n64.drawn(view, &LegendsN64HudPainter::drawBossMeters),
          {{darkPicture("METER_BG1"), {0, 10, 256, 16}, {0, 0, 1, 1}},
           {darkPicture("METER_FG1"), {0, 10, 256, 16}, {0, 0, 1, 1}},
           {darkPicture("METER_BG2"), {256, 10, 256, 16}, {0, 0, 1, 1}},
           {darkPicture("METER_FG2"), {256, 10, 101, 16}, {0, 0, 101.0f / 256, 1}}},
          Mat4{1.0f});
    CHECK(
        Layout::kForeignMeterTop ==
        static_cast<s32>(LegendsN64Frame::toCanvas({0.0f, static_cast<f32>(Layout::kMeterTop)}).y));

    // A meter of one strip, which only Dark Legacy's bosses have, keeps Dark Legacy's crop: at
    // half its health it is not full, as the two-strip rule would have it.
    TextureSet singleArt = meterArt(BossMeter::kPieceWidth, 16);
    const std::array<HealthMeterReading, 1> single{meterReading(1, 12, 20, 250, 500)};
    BossMeters singleMeters;
    showMeters(view, singleMeters, single, singleArt);
    n64.prepare(view);
    REQUIRE(singleMeters.meter(0).look().widths[0] == 112);
    check(n64.drawn(view, &LegendsN64HudPainter::drawBossMeters),
          {{meterPicture(singleArt, n64.device, "METER_BG1"), {0, 10, 256, 16}, {0, 0, 1, 1}},
           {meterPicture(singleArt, n64.device, "METER_FG1"),
            {0, 10, 112, 16},
            {0, 0, 112.0f / 256, 1}}},
          Mat4{1.0f});
    CHECK(n64.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::BossMeters) == 0);
}

TEST_CASE("every boss meter eases by one shared step, a hundredth of the last-bound maximum",
          "[legends][n64-hud]") {
    // FUN_e08887b8: (int)(max * 0.01), set as each meter is bound.
    CHECK(legendsN64MeterStep(1000) == 10);
    CHECK(legendsN64MeterStep(3000) == 30);
    CHECK(legendsN64MeterStep(2550) == 25); // whole health
    CHECK(legendsN64MeterStep(99) == 0);
    CHECK(legendsN64MeterStep(0) == 0);

    LoadedLook n64("n64-hud-meter-ease", kWarriorAndWizard);
    HudView view = n64.viewOf({});
    view.levelHud = kLegendsN64HudStyle;
    TextureSet art = meterArt(LegendsN64HudLayout::kMeterStripWidth, 32);
    // Where each fill is drawn and how wide: with no insets, a meter past half its health fills
    // its first strip and 400 * shown / most - 200 of its second.
    // Whole frame units, as the meters stand and are cropped.
    const auto whole = [](f32 value) { return static_cast<s32>(std::lround(value)); };
    const auto fills = [&] {
        n64.prepare(view);
        std::vector<std::pair<s32, s32>> widths;
        for (const Quad& quad : n64.drawn(view, &LegendsN64HudPainter::drawBossMeters)) {
            if (quad.texture == meterPicture(art, n64.device, "METER_FG1") ||
                quad.texture == meterPicture(art, n64.device, "METER_FG2")) {
                widths.emplace_back(whole(quad.area.x), whole(quad.area.width));
            }
        }
        return widths;
    };
    using Fills = std::vector<std::pair<s32, s32>>;

    // Meters of 1000 and 3000, bound in that order: first sight shows them as they are.
    std::array<HealthMeterReading, 2> readings{meterReading(2, 0, 0, 1000, 1000),
                                               meterReading(2, 0, 0, 3000, 3000)};
    BossMeters meters;
    showMeters(view, meters, readings, art);
    view.seconds = secondsAt(0);
    CHECK(fills() == Fills{{0, 200}, {200, 200}, {256, 200}, {456, 200}});
    // Both lose health: each moves 30 a step, a hundredth of the last bound's 3000.
    readings[0].health = 500;
    readings[1].health = 2000;
    view.bossCount = hudBossReadingsOf(readings, view.bossReadings);
    view.seconds = secondsAt(1);
    CHECK(fills() == Fills{{0, 200}, {200, 188}, {256, 200}, {456, 196}}); // 970, 2970
    view.seconds = secondsAt(3);
    CHECK(fills() == Fills{{0, 200}, {200, 164}, {256, 200}, {456, 188}}); // 910, 2910
    // A rewind shows them as they are.
    view.seconds = secondsAt(1);
    CHECK(fills() == Fills{{0, 200}, {256, 200}, {456, 66}}); // 500, 2000

    // Bound the other way round, both move 10 a step: the last bound is the one of 1000. Bound
    // anew, each shows its boss's health at once.
    std::array<HealthMeterReading, 2> reversed{meterReading(2, 0, 0, 3000, 3000),
                                               meterReading(2, 0, 0, 1000, 1000)};
    BossMeters reboundMeters;
    showMeters(view, reboundMeters, reversed, art);
    view.seconds = secondsAt(2);
    CHECK(fills() == Fills{{0, 200}, {200, 200}, {256, 200}, {456, 200}});
    reversed[0].health = 1500;
    reversed[1].health = 500;
    view.bossCount = hudBossReadingsOf(reversed, view.bossReadings);
    view.seconds = secondsAt(3);
    CHECK(fills() == Fills{{0, 200}, {200, 198}, {256, 200}, {456, 196}}); // 2990, 990

    // A meter under a hundred has a step of nothing, which would never move it: it shows the
    // health as it is.
    std::array<HealthMeterReading, 1> small{meterReading(2, 0, 0, 99, 99)};
    BossMeters smallMeters;
    showMeters(view, smallMeters, small, art);
    view.seconds = secondsAt(4);
    CHECK(fills() == Fills{{0, 200}, {200, 200}});
    small[0].health = 33;
    view.bossCount = hudBossReadingsOf(small, view.bossReadings);
    view.seconds = secondsAt(5);
    CHECK(fills() == Fills{{0, 133}}); // a third of the health: two thirds of the first strip
}

TEST_CASE("N64 help sits on its player's coloured scroll, sized by the N64's steps",
          "[legends][n64-hud]") {
    // FUN_e00717a0: the first width whose wrap holds the widest line, the text no more than
    // three lines of glfm3's 11; 350 for anything else.
    constexpr s32 kLine = 11;
    const Vec2 middle{208, 85};
    const auto widthOf = [&](s32 widest, usize lines) {
        return legendsN64HelpBox(widest, lines, kLine, middle).width;
    };
    CHECK(widthOf(39, 1) == 70);
    CHECK(widthOf(40, 1) == 105);
    CHECK(widthOf(59, 2) == 105);
    CHECK(widthOf(60, 3) == 140);
    CHECK(widthOf(80, 1) == 175);
    CHECK(widthOf(100, 1) == 210);
    CHECK(widthOf(119, 1) == 210);
    CHECK(widthOf(120, 1) == 350);
    CHECK(widthOf(239, 1) == 350);
    CHECK(widthOf(300, 1) == 350);
    // A short message of four lines is too tall for any wrap.
    CHECK(widthOf(10, 4) == 350);
    // As tall as its lines and 16, its text 6 below its top and 12 in for every 70 across.
    using Box = LegendsN64HelpBox;
    CHECK(legendsN64HelpBox(30, 1, kLine, middle) ==
          Box{.x = 173, .y = 72, .width = 70, .height = 27, .textX = 185, .textY = 78});
    for (const auto& [widest, inset] : std::array<std::pair<s32, s32>, 6>{
             {{39, 12}, {59, 18}, {79, 24}, {99, 30}, {119, 36}, {200, 60}}}) {
        CAPTURE(widest);
        const Box box = legendsN64HelpBox(widest, 3, kLine, middle);
        CHECK(box.height == 3 * kLine + 16);
        CHECK(box.textX - box.x == inset);
        CHECK(box.textY - box.y == 6);
    }
    // Kept within the frame's sides, below its first row and above the panels.
    CHECK(legendsN64HelpBox(30, 1, kLine, {-100, 50}).x == 0);
    CHECK(legendsN64HelpBox(30, 1, kLine, {1000, 50}).x == 416 - 70);
    CHECK(legendsN64HelpBox(30, 1, kLine, {200, -50}).y == 1);
    CHECK(legendsN64HelpBox(30, 2, kLine, {200, 500}).y == 170 - (2 * kLine + 16));

    LoadedLook n64("n64-hud-help", kWarriorAndWizard);
    HudView view = n64.viewOf({});
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawHelp).empty());
    // Dark Legacy's message for seat 1, who wears red, over its head: centred 62 above it.
    const std::vector<std::string> lines{"WATCH OUT", "AN EXIT"};
    const Vec2 head{300, 250};
    view.help = HudHelp{.lines = lines, .player = 1, .color = 2, .head = head, .anchored = true};
    // glfm3's "WATCH OUT": eight letters of six and a space of five.
    const Box box = legendsN64HelpBox(53, 2, kLine, LegendsN64Frame::toFrame({300, 250 - 62}));
    REQUIRE(box.width == 105);
    REQUIRE(HelpMessages::kAboveHead == 62);
    const std::vector<Quad> quads = n64.drawn(view, &LegendsN64HudPainter::drawHelp);
    std::vector<Quad> scroll;
    for (const Quad& quad : quads) {
        if (!isText(quad)) {
            scroll.push_back(quad);
        }
        // Opaque, in the frame.
        CHECK(quad.transform == LegendsN64Frame::transform());
    }
    // The three-piece scroll of the player's colour, at the box.
    REQUIRE(scroll.size() == 3);
    for (const Quad& piece : scroll) {
        CHECK(Fixture::pictureOf(piece.texture) == "RED_TEXTSCROLL");
        CHECK(piece.color == Color::white());
    }
    checkRect(covered(scroll), {static_cast<f32>(box.x), static_cast<f32>(box.y),
                                static_cast<f32>(box.width), static_cast<f32>(box.height)});
    // The lines in glfm3, black, from the box's text point, a line height apart.
    CHECK(linesOf(quads, Fixture::kNameFont) ==
          std::vector<Line>{
              {static_cast<f32>(box.textX) + 0.5f, static_cast<f32>(box.textY) + 0.5f, "WATCH OUT"},
              {static_cast<f32>(box.textX) + 0.5f, static_cast<f32>(box.textY + kLine) + 0.5f,
               "AN EXIT"}});
    for (const Quad& quad : quads) {
        if (isText(quad)) {
            CHECK(quad.color == Color::black());
        }
    }
    // Each colour has its scroll, and a message at a place the plain one.
    for (const auto& [color, picture] :
         std::array<std::pair<s32, std::string_view>, 5>{{{0, "YEL_TEXTSCROLL"},
                                                          {1, "BLU_TEXTSCROLL"},
                                                          {3, "GRE_TEXTSCROLL"},
                                                          {-1, "TEXTSCROLL"},
                                                          {7, "TEXTSCROLL"}}}) {
        CAPTURE(color);
        view.help->color = color;
        const std::vector<Quad> drawn = n64.drawn(view, &LegendsN64HudPainter::drawHelp);
        REQUIRE_FALSE(drawn.empty());
        CHECK(Fixture::pictureOf(drawn.front().texture) == picture);
    }
    // The N64's own art: Dark Legacy's look is not asked.
    CHECK(n64.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Help) == 0);
}

TEST_CASE("the N64 ticker and finder are Dark Legacy's counts in N64 fonts, and Dark Legacy's "
          "finder",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-ticker", kWarriorAndWizard, {PickupHud::crystalIcon(2)});
    const Texture* crystal = n64.art.countArt(PickupHud::crystalIcon(2));
    REQUIRE(crystal != nullptr);
    // Seat 1 gathers a crystal and a serpent's fangs; the fangs' icon is not lent.
    PickupHud pickups;
    pickups.showCount(1, PickupHud::crystalIcon(2), 3, 15);
    pickups.showCount(1, PickupHud::kGargoyleIcons[0], 2, 12);
    HudView view =
        n64.viewOf({standing(kWarriorClass, 0, "A"), standing(kWizardClass, 1, "B"), {}, {}});
    view.pickups = &pickups;

    // Each count's text, read from the font 2 glyphs drawn through its own frame: where the
    // frame stands on the canvas, and the line in its units.
    struct Written {
        Vec2 at;
        std::vector<Line> lines;
    };
    const auto countsOf = [&](const std::vector<Quad>& quads) {
        std::vector<Written> counts;
        for (const Quad& quad : quads) {
            if (!Fixture::sheetOf(quad.texture, Fixture::kSmallFont) ||
                quad.transform == LegendsN64Frame::transform()) {
                continue;
            }
            // Font 2 at the frame's proportions, wherever the count stands.
            CHECK(quad.transform[0][0] == LegendsN64Frame::kColumn);
            CHECK(quad.transform[1][1] == LegendsN64Frame::kRow);
            const Vec2 at{quad.transform[3][0], quad.transform[3][1]};
            if (counts.empty() || counts.back().at != at) {
                counts.push_back({at, {}});
            }
            const std::array<Quad, 1> glyph{quad};
            const std::vector<Line> read = linesOf(glyph, Fixture::kSmallFont);
            REQUIRE(read.size() == 1);
            if (counts.back().lines.empty()) {
                counts.back().lines = read;
            } else {
                counts.back().lines.back().text += read.front().text;
            }
        }
        return counts;
    };

    // Over panel 1's middle in canvas units, the first count's top at rest at 282 and the next
    // stacked 20 above it: the icon, 4, then the count in white.
    const f32 centre = LegendsN64Frame::toCanvas({156, 0}).x;
    const f32 written = 4 * 4 * LegendsN64Frame::kColumn; // four glyphs of font 2's four
    const f32 left = centre - (16 + 4 + written) / 2;
    const auto checkTicker = [&](f32 top) {
        const std::vector<Quad> quads = n64.panels(view);
        std::vector<usize> icons;
        std::optional<usize> lastLight;
        for (usize index = 0; index < quads.size(); ++index) {
            if (quads[index].texture == crystal) {
                icons.push_back(index);
            }
            if (Fixture::pictureOf(quads[index].texture) == "TURBOCLR") {
                lastLight = index;
            }
        }
        REQUIRE(icons.size() == 1);
        const Quad& icon = quads[icons.front()];
        checkRect(icon.area, {left, top, 16, 16});
        CHECK(icon.transform == Mat4{1.0f});
        CHECK(icon.color == Color::white());
        // After every panel's last part, the turbo meters' lights.
        REQUIRE(lastLight);
        CHECK(icons.front() > *lastLight);
        const std::vector<Written> counts = countsOf(quads);
        REQUIRE(counts.size() == 2);
        CHECK(counts[0].at.x == Approx(left + 20));
        CHECK(counts[0].at.y == Approx(top + 4));
        CHECK(counts[0].lines == std::vector<Line>{{0.5f, 0.5f, "3/15"}});
        CHECK(counts[1].at.x == Approx(left + 20));
        CHECK(counts[1].at.y == Approx(top - 20 + 4));
        CHECK(counts[1].lines == std::vector<Line>{{0.5f, 0.5f, "2/12"}});
        for (const Quad& quad : quads) {
            if (Fixture::sheetOf(quad.texture, Fixture::kSmallFont)) {
                CHECK(quad.color == Color::white());
            }
        }
    };
    CHECK(Layout::kTickerRestTop == 282);
    checkTicker(static_cast<f32>(Layout::kTickerRestTop));
    // While seat 1's selector names a powerup, its label stands over the panel and the counts
    // stack above it, as Dark Legacy's do.
    Inventory inventory;
    inventory.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, false};
    PowerupSelector selector;
    selector.step(SelectorInput{.up = true}, inventory,
                  PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
    selector.step({}, inventory, 1);
    REQUIRE(selector.showing());
    view.seats[1].selector = &selector;
    checkTicker(static_cast<f32>(PickupHud::countRowY(selector.labelY(StatusBoxPainter::kY))));

    // The runestone finder is Dark Legacy's: the N64 has none.
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawRuneMeter).empty());
    CHECK(n64.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::RuneMeter) == 1);
}
