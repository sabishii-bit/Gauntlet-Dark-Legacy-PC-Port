#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "HudLookArt.h"
#include "LogCapture.h"
#include "RecordedDrawChecks.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64HudFixture.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/n64/hud/LegendsN64Plaque.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
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
#include "game/level/LevelCatalog.h"
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

using test::checkRect;
using test::darkLegacyHourglass;
using test::meterArt;
using test::meterPicture;
using test::meterReading;
using test::Quad;
using test::quadsOf;
using test::showMeters;

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
           Fixture::sheetOf(quad.texture, Fixture::kNumberFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kTitleFont);
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
        const f32 advance = font.advance();
        if (continuing && !lines.empty() && lines.back().y == quad.area.y &&
            (gap == advance || gap == advance + font.spaceAdvance())) {
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

/** Where the first glyph of a line in its own units lands: the N64's unit in, and half a texel
 * more, as TextPainter samples it. */
constexpr Vec2 kFirstGlyph{test::N64Font::kLead + 0.5f, 0.5f};

/** Where a glyph written from pen point (x, y) of panel `slot` lands. */
Line lineAt(usize slot, f32 x, f32 y, std::string text) {
    return {static_cast<f32>(Layout::kSlotX.at(slot)) + x + kFirstGlyph.x, kTop + y + kFirstGlyph.y,
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

/** A picture Dark Legacy's art lends the look: its set, its name and its size. */
struct LentPicture {
    test::DarkLegacyLookup::Set set = test::DarkLegacyLookup::Set::Static;
    std::string_view name;
    u32 width = 16;
    u32 height = 16;
};

/** The N64 look loaded from a synthetic extraction for `party`, with the device it uploads to,
 * Dark Legacy's art it may borrow, lending the count icons `countIcons` and the pictures
 * `pictures`, and the words its panels write. */
struct LoadedLook {
    test::LegendsN64HudFixture files;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    LegendsN64HudPainter look;
    std::vector<std::pair<std::string, const Texture*>> lent; ///< each lent picture, by name

    LoadedLook(std::string_view name, std::span<const HudMember> party,
               std::initializer_list<std::string_view> countIcons = {},
               std::span<const LentPicture> pictures = {})
        : files(name) {
        for (const std::string_view icon : countIcons) {
            art.give(test::DarkLegacyLookup::Set::Count, icon);
        }
        for (const LentPicture& picture : pictures) {
            lent.emplace_back(std::string{picture.name},
                              &art.give(picture.set, picture.name, picture.width, picture.height));
        }
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        REQUIRE(look.load(device, files.root(), art, party));
    }

    /** The picture lent under `name`. */
    const Texture* lentPicture(std::string_view name) const {
        const Texture* found = nullptr;
        for (const auto& [lentName, picture] : lent) {
            if (lentName == name) {
                found = picture;
            }
        }
        REQUIRE(found != nullptr);
        return found;
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

/** `view` at the look's step `step`, readied for its draws. */
void prepareAt(LoadedLook& n64, HudView& view, s32 step) {
    view.seconds = secondsAt(step);
    n64.prepare(view);
}

/** A standing warrior in seat `seat` whose save carries `carried`, its runtime and Dark Legacy's
 * selector over its panel, which the case steps with its own copy of what is carried. */
struct Holder {
    PlayerRuntime runtime;
    PowerupSelector selector;
    Inventory inventory;

    explicit Holder(const Inventory& carried, s32 seat = 0) : inventory(carried) {
        CharacterSave save;
        save.name = "A";
        save.character = kWarriorClass;
        save.color = seat;
        save.progress().inventory = carried;
        runtime.actor.spawn(seat, save, nullptr, Vec3{0}, 0);
    }
    /** Slides the selector up and open, naming the last powerup carried. */
    void open() {
        selector.step(SelectorInput{.up = true}, inventory,
                      PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
        selector.step({}, inventory, 1);
        REQUIRE(selector.showing());
    }
    /** Slides the selector away and closed. */
    void close() {
        selector.step(SelectorInput{.down = true}, inventory, 1);
        selector.step({}, inventory, PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
        selector.step({}, inventory, 1);
        REQUIRE(selector.state() == PowerupSelector::State::Closed);
    }
    /** The seat as the view shows it: costume colour `color`, played here when `local`. */
    HudSeat seat(s32 color, bool local = true) const {
        HudSeat shown = standing(kWarriorClass, color, "A");
        shown.runtime = &runtime;
        shown.selector = &selector;
        shown.local = local;
        return shown;
    }
};

/** The lines `font` wrote along row `y` of panel `slot`. */
std::vector<Line> linesAlong(std::span<const Quad> quads, const test::N64Font& font, usize slot,
                             f32 y) {
    std::vector<Line> found;
    for (const Line& line : linesOf(quads, font)) {
        if (line.y == kTop + y + 0.5f && slotOf({.area = {line.x, line.y, 0, 0}}) == slot) {
            found.push_back(line);
        }
    }
    return found;
}

/** The names of the HUD bank's item icons standing in `slot`, in the order drawn. */
std::vector<std::string_view> iconsIn(std::span<const Quad> quads, usize slot) {
    std::vector<std::string_view> names;
    for (const std::string_view name : pictureOrder(quads, slot)) {
        if (name.starts_with("HUD_")) {
            names.push_back(name);
        }
    }
    return names;
}

/** The quads in `slot` that draw `texture`. */
std::vector<Quad> drawsOf(std::span<const Quad> quads, const Texture* texture, usize slot) {
    std::vector<Quad> found;
    for (const Quad& quad : quads) {
        if (quad.texture == texture && slotOf(quad) == slot) {
            found.push_back(quad);
        }
    }
    return found;
}

/** Where a picture `width` by `height`, drawn at `scale` of its size, stands centred on point
 * `centre` of panel `slot`. */
Rect centredAt(usize slot, LegendsN64Point centre, f32 width, f32 height, f32 scale = 1) {
    return {static_cast<f32>(Layout::kSlotX.at(slot)) + static_cast<f32>(centre.x) -
                width * scale / 2,
            kTop + static_cast<f32>(centre.y) - height * scale / 2, width * scale, height * scale};
}

/** Each arrow's colour in `slot`: the left one, then the right. */
std::pair<Color, Color> arrowsIn(std::span<const Quad> quads, usize slot) {
    const auto arrows = picturesIn(quads, "ARROW", slot);
    REQUIRE(arrows.size() == 2);
    return {arrows[0].color, arrows[1].color};
}

/** An arrow at rest, and one a step lit. */
constexpr Color kArrowResting = Color::white().withAlpha(127);
constexpr Color kArrowLit = Color::white();

/** A party of a warrior and a wizard in the first two seats. */
constexpr std::array<HudMember, 2> kWarriorAndWizard{{
    {.seat = 0, .classIndex = kWarriorClass, .color = 0},
    {.seat = 1, .classIndex = kWizardClass, .color = 1},
}};

/** The lines `font` wrote through frames of their own, as a plaque writes each of its lines:
 * each frame, in turn, with what it wrote in its units. */
std::vector<std::pair<Mat4, std::vector<Line>>> framedLinesOf(std::span<const Quad> quads,
                                                              const test::N64Font& font) {
    std::vector<std::pair<Mat4, std::vector<Quad>>> frames;
    for (const Quad& quad : quads) {
        if (!Fixture::sheetOf(quad.texture, font) ||
            quad.transform == LegendsN64Frame::transform()) {
            continue;
        }
        if (frames.empty() || frames.back().first != quad.transform) {
            frames.emplace_back(quad.transform, std::vector<Quad>{});
        }
        frames.back().second.push_back(quad);
    }
    std::vector<std::pair<Mat4, std::vector<Line>>> lines;
    lines.reserve(frames.size());
    for (const auto& [transform, glyphs] : frames) {
        lines.emplace_back(transform, linesOf(glyphs, font));
    }
    return lines;
}

/** Checks `actual` is the N64's frame carrying a line from frame point `at` at `scale`. */
void checkLineFrame(const Mat4& actual, Vec2 at, f32 scale) {
    const Mat4 expected =
        LegendsN64Frame::transform() *
        glm::scale(glm::translate(Mat4{1.0f}, Vec3{at.x, at.y, 0.0f}), Vec3{scale, scale, 1.0f});
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CAPTURE(column, row);
            CHECK(actual[column][row] == Approx(expected[column][row]).margin(1e-4));
        }
    }
}

/** The quads drawn after the last of the turbo meters' lights. */
std::vector<Quad> afterLights(std::span<const Quad> quads) {
    usize first = 0;
    for (usize index = 0; index < quads.size(); ++index) {
        if (Fixture::pictureOf(quads[index].texture) == "TURBOCLR") {
            first = index + 1;
        }
    }
    REQUIRE(first > 0);
    const std::span<const Quad> rest = quads.subspan(first);
    return {rest.begin(), rest.end()};
}

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
    const StringTable strings;
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
    const auto left = static_cast<f32>(Layout::kSlotX[1]);
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

    // Health is "%4d" in numfont from (41,9), held to four digits, its padding numfont's spaces.
    const auto health = linesOf(quads, Fixture::kNumberFont);
    const f32 space = Fixture::kNumberFont.spaceAdvance();
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
        f32 width = 0;
        for (const char code : text) {
            width +=
                code == ' ' ? Fixture::kSmallFont.spaceAdvance() : Fixture::kSmallFont.advance();
        }
        return static_cast<s32>(width);
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
          std::vector<Line>{lineAt(3, 41 + 3 * Fixture::kNumberFont.spaceAdvance(), 9, "0")});
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
    using Set = test::DarkLegacyLookup::Set;
    const test::LegendsN64HudFixture files("n64-hud-borrowed");
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    // Dark Legacy's art stands ready to lend what a look might ask of it, and a crystal's
    // count icon.
    struct Lent {
        test::DarkLegacyLookup lookup;
        const Texture* picture = nullptr;
    };
    std::vector<Lent> lent;
    for (const Set set : {Set::Static, Set::Select, Set::Count, Set::Inventory}) {
        for (const std::string_view name : {"RUNE13", "SM_KEY_BLU", "S4_WAR", "SHP_XRAY_GLASSES"}) {
            lent.push_back({{set, std::string{name}}, &art.give(set, name)});
        }
    }
    const Texture& crystal = art.give(Set::Count, PickupHud::crystalIcon(1));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LegendsN64HudPainter look;
    REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
    // As it opens it takes every icon Dark Legacy's pickup counts may show; INVENTORY's pictures
    // of the powerups the N64 has no icon for and SELECT's of the legend items; STATIC's
    // thirteenth rune, its twelve runestones and its four keys; and nothing else: the panels,
    // the hourglass, the help and the meters draw the N64's own art.
    std::vector<test::DarkLegacyLookup> borrowed;
    for (const std::string& icon : PickupHud::countIcons()) {
        borrowed.push_back({Set::Count, icon});
    }
    for (const std::string_view name :
         {"SHP_GASMASK", "SHP_XRAY_GLASSES", "SHP_POJOEGG", "SHP_TURBO_BOOST", "SHP_MIKEY",
          "SHP_HANDOFDEATH", "SHP_HEALTHVAMPIRE"}) {
        borrowed.push_back({Set::Inventory, std::string{name}});
    }
    for (const std::string_view name : {"LAMP", "BILLOWS", "BOOK", "LANTERN"}) {
        borrowed.push_back({Set::Select, std::string{name}});
    }
    borrowed.push_back({Set::Static, "RUNE13"});
    for (const std::string_view colour : {"BLU", "RED", "YEL", "GRE"}) {
        for (s32 rune = 1; rune <= 3; ++rune) {
            borrowed.push_back({Set::Static, std::format("SM_RUNE_{}_0{}", colour, rune)});
        }
    }
    for (const std::string_view colour : {"BLU", "RED", "YEL", "GRE"}) {
        borrowed.push_back({Set::Static, std::format("SM_KEY_{}", colour)});
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
    // uploaded while drawing. Seat 0 carries keys, a potion and an X-ray powerup, its selector
    // up over its panel, standing on an N64 exit; seat 3, played here, browses to its quest
    // page, holding the thirteenth rune, a scimitar and the first boss's key, standing on an
    // exit of Dark Legacy's.
    Inventory carried;
    carried.powerups[0] = PowerupSlot{120, powerup::kSpecial, 0, powerup::kXRay, true};
    Holder holder(carried, 0);
    holder.open();
    HudSeat carrying = holder.seat(0);
    carrying.box.health = 250;
    carrying.box.keys = 2;
    carrying.box.potions = 1;
    carrying.box.potionKind = 3;
    carrying.pad = HudPad{.identity = 0x0101, .nativeCode = 0x0101, .destination = nullptr};
    LevelRef courtyard;
    courtyard.realmId = 1;
    courtyard.title = "Castle Courtyard";
    HudSeat questing = standing(kWarriorClass, 3, "D");
    questing.local = true;
    questing.legends = 0x2; // realm 1's
    questing.box.runes = StatusBoxPainter::kThirteenthRune;
    questing.box.bossKeys = 1;
    questing.pad = HudPad{.identity = kDarkLegacyPadIdentityBase,
                          .nativeCode = std::nullopt,
                          .destination = &courtyard};
    PickupHud pickups;
    pickups.showCount(0, PickupHud::crystalIcon(1), 3, 15);
    const std::vector<std::string> lines{"WATCH OUT"};
    HudView view;
    view.seats = {carrying, fallen(standing(kWizardClass, 1, "B"), true, true),
                  fallen(standing(kWizardClass, 2, "C"), false), questing};
    view.strings = &strings;
    view.pickups = &pickups;
    view.help = HudHelp{.lines = lines, .player = 0, .color = 0, .head = {256, 300}};
    view.hourglass = ChallengeHud::Look{.elapsed = 0.5f, .fallingFrame = 2};
    view.bossMeters = &meters;
    view.bossCount = hudBossReadingsOf(dragon, view.bossReadings);
    view.levelHud = kLegendsN64HudStyle;
    const auto frameAt = [&](s32 step) {
        view.seconds = secondsAt(step);
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
    };
    frameAt(0);
    look.browse(3, SelectorInput{.down = true});
    look.browse(3, SelectorInput{.down = true});
    device.draws.clear();
    for (s32 step = 1; step < 3; ++step) {
        frameAt(step);
    }
    CHECK(art.lookups == borrowed);
    CHECK(device.texturesCreated == uploaded);
    REQUIRE_FALSE(device.draws.empty());
    // It draws its own art, the meter's and the pictures it took: the count's crystal, the
    // X-ray's, the thirteenth rune's and the key's, and none other of Dark Legacy's.
    const std::array<test::DarkLegacyLookup, 3> taken{{{Set::Inventory, "SHP_XRAY_GLASSES"},
                                                       {Set::Static, "RUNE13"},
                                                       {Set::Static, "SM_KEY_BLU"}}};
    const auto drawn = [&](const Texture* picture) {
        return std::ranges::any_of(
            device.draws, [&](const test::RecordedDraw& draw) { return draw.texture == picture; });
    };
    for (const Lent& given : lent) {
        CAPTURE(given.lookup.name, static_cast<s32>(given.lookup.set));
        CHECK(drawn(given.picture) == (std::ranges::find(taken, given.lookup) != taken.end()));
    }
    CHECK(drawn(&crystal));
    // The plaques were among it: the N64's own picture, and the port's words in font 8.
    CHECK(std::ranges::any_of(device.draws, [](const test::RecordedDraw& draw) {
        return Fixture::pictureOf(draw.texture) == "LNAME_0101";
    }));
    CHECK(std::ranges::any_of(device.draws, [](const test::RecordedDraw& draw) {
        return Fixture::sheetOf(draw.texture, Fixture::kTitleFont);
    }));
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
    // The runestone finder is Dark Legacy's, prepared as its look prepares it; the panels with
    // their carousels, the hourglass, the boss meters and the help are the N64's own, and no
    // selector's label rises over the panels.
    const test::FakeHudRecord& darkLegacy = n64.art.record;
    for (const test::FakeHudPart part :
         {test::FakeHudPart::Prepare, test::FakeHudPart::RuneMeter}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 1);
    }
    for (const test::FakeHudPart part :
         {test::FakeHudPart::Status, test::FakeHudPart::Hourglass, test::FakeHudPart::BossMeters,
          test::FakeHudPart::Selectors, test::FakeHudPart::Help}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 0);
    }
    CHECK(darkLegacy.calls.size() == 2);
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

    // Below half its health the dragon's first strip fills from its cap, its type's 46 at
    // 200/256, over its share of the rest, and its second strip shows no fill; once the dragon
    // is dead its meter goes.
    LoadedLook hurt("n64-hud-meters-hurt", kWarriorAndWizard);
    HudView hurtView = hurt.viewOf({});
    hurtView.levelHud = kLegendsN64HudStyle;
    TextureSet hurtArt = meterArt(LegendsN64HudLayout::kMeterStripWidth, 32);
    const std::array<HealthMeterReading, 1> hurtDragon{meterReading(2, 46, 72, 625, 2500)};
    BossMeters hurtMeters;
    showMeters(hurtView, hurtMeters, hurtDragon, hurtArt);
    hurt.prepare(hurtView);
    const auto hurtPicture = [&](std::string_view name) {
        return meterPicture(hurtArt, hurt.device, name);
    };
    // 35.9375 + 0.5 * (200 - 35.9375), cut to the whole unit.
    check(hurt.drawn(hurtView, &LegendsN64HudPainter::drawBossMeters),
          {{hurtPicture("METER_BG1"), {0, 1, 200, 32}, {0, 0, 1, 1}},
           {hurtPicture("METER_FG1"), {0, 1, 117, 32}, {0, 0, 117.0f / 200, 1}},
           {hurtPicture("METER_BG2"), {200, 1, 200, 32}, {0, 0, 1, 1}}},
          LegendsN64Frame::transform());
    hurtMeters.update(0, hurtDragon, false, false);
    hurt.prepare(hurtView);
    CHECK(hurt.drawn(hurtView, &LegendsN64HudPainter::drawBossMeters).empty());
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
    // glfm3's "WATCH OUT": eight letters of six and a space of five, each with the unit the N64
    // writes before it, 62 across: past the 105 scroll's wrap of 60, so the 140.
    const Box box = legendsN64HelpBox(62, 2, kLine, LegendsN64Frame::toFrame({300, 250 - 62}));
    REQUIRE(box.width == 140);
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
          std::vector<Line>{{static_cast<f32>(box.textX) + kFirstGlyph.x,
                             static_cast<f32>(box.textY) + kFirstGlyph.y, "WATCH OUT"},
                            {static_cast<f32>(box.textX) + kFirstGlyph.x,
                             static_cast<f32>(box.textY + kLine) + kFirstGlyph.y, "AN EXIT"}});
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
    // Four glyphs of font 2, each its four columns and the N64's unit.
    const f32 written = 4 * Fixture::kSmallFont.advance() * LegendsN64Frame::kColumn;
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
        CHECK(counts[0].lines == std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "3/15"}});
        CHECK(counts[1].at.x == Approx(left + 20));
        CHECK(counts[1].at.y == Approx(top - 20 + 4));
        CHECK(counts[1].lines == std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "2/12"}});
        for (const Quad& quad : quads) {
            if (Fixture::sheetOf(quad.texture, Fixture::kSmallFont)) {
                CHECK(quad.color == Color::white());
            }
        }
    };
    CHECK(Layout::kTickerRestTop == 282);
    checkTicker(static_cast<f32>(Layout::kTickerRestTop));
    // While seat 1's selector names a powerup its carousel shows in the panel, and no label
    // rises over it: the counts keep their place.
    Inventory inventory;
    inventory.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, false};
    PowerupSelector selector;
    selector.step(SelectorInput{.up = true}, inventory,
                  PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
    selector.step({}, inventory, 1);
    REQUIRE(selector.showing());
    view.seats[1].selector = &selector;
    checkTicker(static_cast<f32>(Layout::kTickerRestTop));

    // The runestone finder is Dark Legacy's: the N64 has none.
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawRuneMeter).empty());
    CHECK(n64.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::RuneMeter) == 1);
}

TEST_CASE("down steps a closed selector's panel through the stats and quest pages, and opening "
          "the selector shows the carousel",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-pages", kWarriorAndWizard);
    // Seat 0, played here, carries an invisibility and a key and holds the scimitar; seat 1,
    // played here too, holds nothing for the quest page; seat 2 is another machine's.
    Inventory carried;
    carried.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, true};
    Holder first(carried, 0);
    Holder other(carried, 2);
    HudSeat holding = first.seat(0);
    holding.legends = 0x2; // realm 1's
    holding.box.keys = 1;
    HudSeat empty = standing(kWizardClass, 1, "B");
    empty.local = true;
    HudView view = n64.viewOf({holding, empty, other.seat(2, false), {}});

    // What a panel shows, told by what it draws: the normal page names its player beside the
    // portrait, the stats page writes a stat in the name's place, and the carousel and the quest
    // page stand where the portrait was.
    enum class Shown : u8 { Normal, Stats, Items, Neither };
    const auto shownIn = [](std::span<const Quad> quads, usize slot) {
        const bool named = !linesAlong(quads, Fixture::kNameFont, slot, 30).empty();
        const bool stat = !linesAlong(quads, Fixture::kSmallFont, slot, 28).empty();
        const bool portrait = !picturesIn(quads, slot == 1 ? "WIZ_P1" : "WAR_P1", slot).empty();
        if (portrait && named && !stat) {
            return Shown::Normal;
        }
        if (portrait && stat && !named) {
            return Shown::Stats;
        }
        return !portrait && !named && !stat ? Shown::Items : Shown::Neither;
    };
    using Pages = std::array<Shown, 3>;
    std::vector<Quad> quads;
    const auto pagesAt = [&](s32 step) {
        prepareAt(n64, view, step);
        quads = n64.panels(view);
        return Pages{shownIn(quads, 0), shownIn(quads, 1), shownIn(quads, 2)};
    };
    const Color dimmed = Color::white().withAlpha(135);
    const auto keyColour = [&] {
        const auto key = picturesIn(quads, "KEY1", 0);
        REQUIRE(key.size() == 1);
        return key.front().color;
    };

    CHECK(pagesAt(0) == Pages{Shown::Normal, Shown::Normal, Shown::Normal});
    n64.look.browse(0, SelectorInput{.down = true});
    CHECK(pagesAt(1) == Pages{Shown::Stats, Shown::Normal, Shown::Normal});
    // Down lights no arrow; the stats page leaves the key as it is.
    CHECK(arrowsIn(quads, 0) == std::pair{kArrowResting, kArrowResting});
    CHECK(keyColour() == Color::white());
    n64.look.browse(0, SelectorInput{.down = true});
    CHECK(pagesAt(2) == Pages{Shown::Items, Shown::Normal, Shown::Normal});
    // The quest page centres the scimitar, in the N64's own icon, and dims the key.
    CHECK(iconsIn(quads, 0) == std::vector<std::string_view>{"HUD_SCIMITAR"});
    checkRect(picturesIn(quads, "HUD_SCIMITAR", 0).front().area,
              centredAt(0, Layout::kSelected, 28, 28));
    CHECK(keyColour() == dimmed);
    n64.look.browse(0, SelectorInput{.down = true});
    CHECK(pagesAt(3) == Pages{Shown::Normal, Shown::Normal, Shown::Normal});
    // Seat 1 holds nothing for the quest page: down passes it by.
    n64.look.browse(1, SelectorInput{.down = true});
    CHECK(pagesAt(4) == Pages{Shown::Normal, Shown::Stats, Shown::Normal});
    n64.look.browse(1, SelectorInput{.down = true});
    CHECK(pagesAt(5) == Pages{Shown::Normal, Shown::Normal, Shown::Normal});

    // Seat 0 on its stats page: its selector sliding up shows the carousel at once, and once
    // the selector is down again the panel is back on its normal page.
    n64.look.browse(0, SelectorInput{.down = true});
    CHECK(pagesAt(6) == Pages{Shown::Stats, Shown::Normal, Shown::Normal});
    first.selector.step(SelectorInput{.up = true}, first.inventory, 1);
    REQUIRE(first.selector.state() == PowerupSelector::State::SlidingIn);
    CHECK(pagesAt(7) == Pages{Shown::Items, Shown::Normal, Shown::Normal});
    CHECK(iconsIn(quads, 0) == std::vector<std::string_view>{"HUD_INVIS"});
    CHECK(keyColour() == dimmed);
    first.selector.step({}, first.inventory,
                        PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
    first.selector.step({}, first.inventory, 1);
    REQUIRE(first.selector.showing());
    CHECK(pagesAt(8) == Pages{Shown::Items, Shown::Normal, Shown::Normal});
    first.close();
    CHECK(pagesAt(9) == Pages{Shown::Normal, Shown::Normal, Shown::Normal});
    // Every seat's selector shows its carousel, the other machine's player's too.
    other.open();
    CHECK(pagesAt(10) == Pages{Shown::Normal, Shown::Normal, Shown::Items});
    CHECK(iconsIn(quads, 2) == std::vector<std::string_view>{"HUD_INVIS"});

    // A dying player's panel keeps its normal page (FUN_e0059114), whether it was on another
    // page or its selector stands open, and is on its normal page once it stands again.
    n64.look.browse(0, SelectorInput{.down = true});
    CHECK(pagesAt(11) == Pages{Shown::Stats, Shown::Normal, Shown::Items});
    view.seats[0] = fallen(holding, false);
    CHECK(pagesAt(12) == Pages{Shown::Normal, Shown::Normal, Shown::Items});
    first.open();
    CHECK(pagesAt(13) == Pages{Shown::Normal, Shown::Normal, Shown::Items});
    CHECK(iconsIn(quads, 0).empty());
    first.close();
    view.seats[0] = holding;
    CHECK(pagesAt(14) == Pages{Shown::Normal, Shown::Normal, Shown::Items});
}

TEST_CASE("left and right on a closed panel enter the stat ring at either end and leave it past "
          "either end",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-ring", kWarriorAndWizard);
    HudSeat seat = standing(kWarriorClass, 0, "A");
    seat.local = true;
    seat.stats.values = {34, 45, 78, 56};
    seat.box.level = 21;
    seat.legends = 0x2 | 0x4 | 0x20; // realms 1, 2 and 5: the scimitar, the ice axe, the saviour
    HudView view = n64.viewOf({seat, {}, {}, {}});
    s32 step = 0;
    prepareAt(n64, view, step);
    // The panel after a press, drawn in the step it is first shown in.
    const auto press = [&](const SelectorInput& presses) {
        n64.look.browse(0, presses);
        prepareAt(n64, view, ++step);
        return n64.panels(view);
    };
    const auto stat = [](std::span<const Quad> quads) {
        return linesAlong(quads, Fixture::kSmallFont, 0, 28);
    };
    const auto named = [](std::span<const Quad> quads) {
        return linesAlong(quads, Fixture::kNameFont, 0, 30);
    };
    // "LEVEL" stands three of font 2's spaces in from the stats' 8.
    const f32 level = 8 + 3 * Fixture::kSmallFont.spaceAdvance();
    using Lines = std::vector<Line>;
    const auto lit = std::pair{kArrowResting, kArrowLit};
    const auto litLeft = std::pair{kArrowLit, kArrowResting};
    const auto resting = std::pair{kArrowResting, kArrowResting};

    // Right enters the stats at the first, strength, lighting the right arrow for the step the
    // stat is first shown in.
    std::vector<Quad> quads = press(SelectorInput{.right = true});
    CHECK(stat(quads) == Lines{lineAt(0, 8, 28, "STRENGTH 000034")});
    CHECK(named(quads).empty());
    CHECK(arrowsIn(quads, 0) == lit);
    n64.prepare(view);
    CHECK(arrowsIn(n64.panels(view), 0) == lit);
    prepareAt(n64, view, ++step);
    CHECK(arrowsIn(n64.panels(view), 0) == resting);
    // Seven more reach the last, the level; one more is past it: the normal page again.
    for (s32 more = 0; more < 7; ++more) {
        quads = press(SelectorInput{.right = true});
    }
    CHECK(stat(quads) == Lines{lineAt(0, level, 28, "LEVEL 000021")});
    quads = press(SelectorInput{.right = true});
    CHECK(stat(quads).empty());
    CHECK(named(quads) == Lines{lineAt(0, 25, 30, "A")});
    CHECK(arrowsIn(quads, 0) == lit);
    // Left enters at the level and leaves past strength.
    quads = press(SelectorInput{.left = true});
    CHECK(stat(quads) == Lines{lineAt(0, level, 28, "LEVEL 000021")});
    CHECK(arrowsIn(quads, 0) == litLeft);
    for (s32 more = 0; more < 7; ++more) {
        quads = press(SelectorInput{.left = true});
    }
    CHECK(stat(quads) == Lines{lineAt(0, 8, 28, "STRENGTH 000034")});
    quads = press(SelectorInput{.left = true});
    CHECK(stat(quads).empty());
    CHECK(named(quads) == Lines{lineAt(0, 25, 30, "A")});

    // On the quest page left and right go round its entries, each the one before it and the
    // one after it beside it; for the step a move first shows in, the icons go.
    press(SelectorInput{.down = true});
    quads = press(SelectorInput{.down = true});
    using Icons = std::vector<std::string_view>;
    CHECK(iconsIn(quads, 0) == Icons{"HUD_SAVIOUR", "HUD_SCIMITAR", "HUD_ICEAXE"});
    const auto settled = [&] {
        prepareAt(n64, view, ++step);
        return n64.panels(view);
    };
    quads = press(SelectorInput{.right = true});
    CHECK(iconsIn(quads, 0).empty());
    CHECK(arrowsIn(quads, 0) == lit);
    quads = settled();
    CHECK(iconsIn(quads, 0) == Icons{"HUD_SCIMITAR", "HUD_ICEAXE", "HUD_SAVIOUR"});
    CHECK(arrowsIn(quads, 0) == resting);
    press(SelectorInput{.right = true});
    CHECK(iconsIn(settled(), 0) == Icons{"HUD_ICEAXE", "HUD_SAVIOUR", "HUD_SCIMITAR"});
    press(SelectorInput{.right = true});
    CHECK(iconsIn(settled(), 0) == Icons{"HUD_SAVIOUR", "HUD_SCIMITAR", "HUD_ICEAXE"});
    quads = press(SelectorInput{.left = true});
    CHECK(arrowsIn(quads, 0) == litLeft);
    CHECK(iconsIn(settled(), 0) == Icons{"HUD_ICEAXE", "HUD_SAVIOUR", "HUD_SCIMITAR"});
}

TEST_CASE("the stats page pairs each N64 word with its own value", "[legends][n64-hud]") {
    // e00aad34's "%8.8s %6.6d": the name right-aligned in eight and cut to them; e00aad40's
    // "%s " at nought.
    CHECK(legendsN64StatLine("Strength", 34) == "Strength 000034");
    CHECK(legendsN64StatLine("Speed", 45) == "   Speed 000045");
    CHECK(legendsN64StatLine("Exp.", 1234567) == "    Exp. 1234567");
    CHECK(legendsN64StatLine("Strengths", 7) == "Strength 000007");
    CHECK(legendsN64StatLine("Speed", 0) == "Speed ");

    LoadedLook n64("n64-hud-stats", kWarriorAndWizard);
    // Every stat a value of its own, so that a word paired with another's shows.
    HudSeat seat = standing(kWarriorClass, 0, "A");
    seat.local = true;
    seat.stats.values = {34, 45, 78, 56}; // strength, speed, armour, magic
    seat.experience = 12345;
    seat.box.gold = 6789;
    seat.box.level = 21;
    // Seat 1's speed is nothing.
    HudSeat slow = standing(kWizardClass, 1, "B");
    slow.local = true;
    slow.stats.values = {12, 0, 34, 56};
    HudView view = n64.viewOf({seat, slow, {}, {}});
    prepareAt(n64, view, 0);
    // FUN_e0055784's ring, each word in font 2 from (8,28), its padding the font's spaces of 3
    // and their units, with the class still under it.
    const auto padded = [](s32 spaces) {
        return 8 + static_cast<f32>(spaces) * Fixture::kSmallFont.spaceAdvance();
    };
    const std::array<Line, 8> ring{
        lineAt(0, padded(0), 28, "STRENGTH 000034"), lineAt(0, padded(3), 28, "SPEED 000045"),
        lineAt(0, padded(3), 28, "MAGIC 000056"),    lineAt(0, padded(2), 28, "ARMOUR 000078"),
        lineAt(0, padded(0), 28, "MAX HLTH 000600"), lineAt(0, padded(4), 28, "EXP. 012345"),
        lineAt(0, padded(4), 28, "GOLD 006789"),     lineAt(0, padded(3), 28, "LEVEL 000021"),
    };
    for (usize place = 0; place < ring.size(); ++place) {
        CAPTURE(place);
        n64.look.browse(0, SelectorInput{.right = true});
        if (place < 2) {
            n64.look.browse(1, SelectorInput{.right = true});
        }
        prepareAt(n64, view, static_cast<s32>(place) + 1);
        const std::vector<Quad> quads = n64.panels(view);
        CHECK(linesAlong(quads, Fixture::kSmallFont, 0, 28) == std::vector<Line>{ring.at(place)});
        CHECK(linesAlong(quads, Fixture::kSmallFont, 0, 40) ==
              std::vector<Line>{lineAt(0, 20, 40, "WARRIOR")});
        if (place == 1) {
            // A stat at nought is its word alone, unpadded.
            CHECK(linesAlong(quads, Fixture::kSmallFont, 1, 28) ==
                  std::vector<Line>{lineAt(1, 8, 28, "SPEED")});
        }
    }
}

TEST_CASE("the carousel shows the selection between its neighbours, dims what is off and blinks "
          "on a move",
          "[legends][n64-hud]") {
    // What it writes under the selection: Dark Legacy's seconds or charges left, whole, kept
    // for a powerup taken off; nothing for one worn for good or with nought left.
    const auto timed = [](f32 left, bool on) {
        return PowerupSlot{left, powerup::kSpecial, 0, powerup::kInvisible, on};
    };
    CHECK(legendsN64CarouselCount(timed(20.7f, true)) == 20);
    CHECK(legendsN64CarouselCount(timed(20.7f, false)) == 20);
    CHECK_FALSE(legendsN64CarouselCount(timed(0.5f, true)).has_value());
    CHECK_FALSE(legendsN64CarouselCount(timed(-1, true)).has_value());
    CHECK_FALSE(legendsN64CarouselCount(timed(0, true)).has_value());
    CHECK(legendsN64CarouselCount(timed(123456, true)) == 9999);
    CHECK(legendsN64CarouselCount(PowerupSlot{-1, powerup::kWeapon, 3, damage::kHammer, true}) ==
          3);
    CHECK_FALSE(
        legendsN64CarouselCount(PowerupSlot{-1, powerup::kWeapon, 0.5f, damage::kHammer, true})
            .has_value());

    // Dark Legacy's Pojo has no N64 icon: its INVENTORY picture, 32 square, is fitted into the
    // N64's 28.
    const std::array<LentPicture, 1> lend{{{.set = test::DarkLegacyLookup::Set::Inventory,
                                            .name = "SHP_POJOEGG",
                                            .width = 32,
                                            .height = 32}}};
    LoadedLook n64("n64-hud-carousel", kWarriorAndWizard, {}, lend);
    const Texture* pojo = n64.lentPicture("SHP_POJOEGG");
    Inventory carried;
    carried.powerups[0] = timed(30.5f, true);
    carried.powerups[1] = PowerupSlot{20.7f, powerup::kArmor, 0,
                                      armor::kInvulnerable | armor::kGoldInvulnerable, false};
    carried.powerups[2] = PowerupSlot{60, powerup::kSpecial, 0, powerup::kPojo, true};
    carried.powerups[3] = PowerupSlot{-1, powerup::kWeapon, 3, damage::kHammer, true};
    Holder holder(carried, 0);
    holder.open();
    REQUIRE(holder.selector.selection() == 3);
    HudSeat seat = holder.seat(0);
    seat.box.keys = 2;
    seat.box.potions = 1;
    seat.box.potionKind = 2;
    HudView view = n64.viewOf({seat, {}, {}, {}});
    const auto itemsIn = [&](std::span<const Quad> quads) {
        std::vector<Quad> items;
        for (const Quad& quad : quads) {
            if (slotOf(quad) == 0 &&
                (Fixture::pictureOf(quad.texture).starts_with("HUD_") || quad.texture == pojo)) {
                items.push_back(quad);
            }
        }
        return items;
    };
    const auto check = [](const Quad& quad, std::string_view picture, const Rect& area,
                          Color color) {
        CAPTURE(picture);
        CHECK(Fixture::pictureOf(quad.texture) == picture);
        checkRect(quad.area, area);
        CHECK(quad.color == color);
    };
    const Color off = Color::white().withAlpha(127);
    const auto countIn = [](std::span<const Quad> quads) {
        std::vector<std::pair<Line, Color>> written;
        for (const Line& line : linesAlong(quads, Fixture::kSmallFont, 0, 40)) {
            for (const Quad& quad : quads) {
                if (quad.area.x == line.x && quad.area.y == line.y && isText(quad)) {
                    written.emplace_back(line, quad.color);
                }
            }
        }
        return written;
    };
    using Count = std::vector<std::pair<Line, Color>>;

    // The thunder hammer named, Pojo before it and invisibility after it, going round: the
    // neighbours at 0.6 of their size, the hammer's 32 whole, Pojo's picture fitted into 28.
    prepareAt(n64, view, 0);
    std::vector<Quad> quads = n64.panels(view);
    std::vector<Quad> items = itemsIn(quads);
    REQUIRE(items.size() == 3);
    CHECK(items[0].texture == pojo);
    checkRect(items[0].area, centredAt(0, Layout::kPrevious, 28, 28, 0.6f));
    CHECK(items[0].color == Color::white());
    check(items[1], "HUD_HAMMER", centredAt(0, Layout::kSelected, 32, 32), Color::white());
    check(items[2], "HUD_INVIS", centredAt(0, Layout::kNext, 28, 28, 0.6f), Color::white());
    // Its charges left, "%4d" in font 2 from (64,40), over the panel's own art, padded with the
    // font's spaces.
    const auto count = [](s32 digits) {
        return 64 + static_cast<f32>(4 - digits) * Fixture::kSmallFont.spaceAdvance();
    };
    CHECK(countIn(quads) == Count{{lineAt(0, count(1), 40, "3"), Color::white()}});
    // The carousel stands where the portrait, the name and the class were, and dims the key
    // and the bottle.
    CHECK(picturesIn(quads, "WAR_P1", 0).empty());
    CHECK(linesAlong(quads, Fixture::kNameFont, 0, 30).empty());
    CHECK(linesAlong(quads, Fixture::kSmallFont, 0, 40).size() == 1);
    CHECK(picturesIn(quads, "KEY1", 0).front().color == Color::white().withAlpha(135));
    CHECK(picturesIn(quads, "POTIONB", 0).front().color == Color::white().withAlpha(135));
    CHECK(arrowsIn(quads, 0) == std::pair{kArrowResting, kArrowResting});
    // The N64 writes no powerup's name over its panels.
    CHECK(n64.drawn(view, &LegendsN64HudPainter::drawSelectors).empty());

    // A press as the scene takes it: the look is told of it as it is sampled, and Dark Legacy's
    // selector then takes it.
    const auto press = [&](const SelectorInput& presses) {
        n64.look.steer(0, presses);
        holder.selector.step(presses, holder.inventory, 1);
    };

    // Left moves Dark Legacy's selection to Pojo: for the step that shows it, the icons and the
    // count go and the left arrow is lit.
    press(SelectorInput{.left = true});
    REQUIRE(holder.selector.selection() == 2);
    prepareAt(n64, view, 1);
    quads = n64.panels(view);
    CHECK(itemsIn(quads).empty());
    CHECK(countIn(quads).empty());
    CHECK(arrowsIn(quads, 0) == std::pair{kArrowLit, kArrowResting});
    // A step on they are back: the gold invulnerability before it, taken off and so at
    // transparency 0x80, and the hammer after it.
    prepareAt(n64, view, 2);
    quads = n64.panels(view);
    items = itemsIn(quads);
    REQUIRE(items.size() == 3);
    check(items[0], "HUD_INVULNG", centredAt(0, Layout::kPrevious, 28, 28, 0.6f), off);
    CHECK(items[1].texture == pojo);
    checkRect(items[1].area, centredAt(0, Layout::kSelected, 28, 28));
    check(items[2], "HUD_HAMMER", centredAt(0, Layout::kNext, 32, 32, 0.6f), Color::white());
    CHECK(countIn(quads) == Count{{lineAt(0, count(2), 40, "60"), Color::white()}});
    CHECK(arrowsIn(quads, 0) == std::pair{kArrowResting, kArrowResting});
    // The gold invulnerability named: it and what it has left are dimmed.
    press(SelectorInput{.left = true});
    prepareAt(n64, view, 3);
    prepareAt(n64, view, 4);
    quads = n64.panels(view);
    items = itemsIn(quads);
    REQUIRE(items.size() == 3);
    check(items[0], "HUD_INVIS", centredAt(0, Layout::kPrevious, 28, 28, 0.6f), Color::white());
    check(items[1], "HUD_INVULNG", centredAt(0, Layout::kSelected, 28, 28), off);
    CHECK(items[2].texture == pojo);
    CHECK(countIn(quads) == Count{{lineAt(0, count(2), 40, "20"), off}});
    // Right goes back, lighting the right arrow.
    press(SelectorInput{.right = true});
    prepareAt(n64, view, 5);
    CHECK(arrowsIn(n64.panels(view), 0) == std::pair{kArrowResting, kArrowLit});

    // Of two powerups only the one before stands beside the selection; of one, neither.
    Inventory two;
    two.powerups[0] = timed(30, true);
    two.powerups[1] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kLevitation, true};
    Holder pair(two, 1);
    pair.open();
    Inventory one;
    one.powerups[4] = timed(30, true);
    Holder single(one, 2);
    single.open();
    view.seats[1] = pair.seat(1);
    view.seats[2] = single.seat(2);
    prepareAt(n64, view, 6);
    quads = n64.panels(view);
    CHECK(iconsIn(quads, 1) == std::vector<std::string_view>{"HUD_INVIS", "HUD_LEVITATE"});
    checkRect(picturesIn(quads, "HUD_LEVITATE", 1).front().area,
              centredAt(1, Layout::kSelected, 28, 28));
    CHECK(iconsIn(quads, 2) == std::vector<std::string_view>{"HUD_INVIS"});
    checkRect(picturesIn(quads, "HUD_INVIS", 2).front().area,
              centredAt(2, Layout::kSelected, 28, 28));
}

TEST_CASE("an open selector's press lights its own arrow, whatever comes up",
          "[legends][n64-hud]") {
    // FUN_e00496d4 lights the arrow of every press the carousel takes. Seat 0, played here,
    // carries two powerups, each the other's neighbour both ways; seat 1, played here, one;
    // seat 2, another machine's, three, its presses never told to the look.
    LoadedLook n64("n64-hud-carousel-presses", kWarriorAndWizard);
    const auto timed = [](u32 special) {
        return PowerupSlot{30, powerup::kSpecial, 0, special, true};
    };
    Inventory two;
    two.powerups[0] = timed(powerup::kInvisible);
    two.powerups[1] = timed(powerup::kLevitation);
    Inventory one;
    one.powerups[4] = timed(powerup::kInvisible);
    Inventory three = two;
    three.powerups[2] = timed(powerup::kGrowth);
    Holder pair(two, 0);
    Holder single(one, 1);
    Holder remote(three, 2);
    for (Holder* holder : {&pair, &single, &remote}) {
        holder->open();
    }
    HudView view = n64.viewOf({pair.seat(0), single.seat(1), remote.seat(2, false), {}});
    s32 step = 0;
    prepareAt(n64, view, step);
    // A press as the scene takes it, told to the look for a seat played here, and the panels in
    // the step it first shows in; then the panels a step on.
    const auto press = [&](Holder& holder, s32 seat, const SelectorInput& presses) {
        if (view.seats.at(static_cast<usize>(seat)).local) {
            n64.look.steer(seat, presses);
        }
        holder.selector.step(presses, holder.inventory, 1);
        prepareAt(n64, view, ++step);
        return n64.panels(view);
    };
    const auto settled = [&] {
        prepareAt(n64, view, ++step);
        return n64.panels(view);
    };
    const auto litLeft = std::pair{kArrowLit, kArrowResting};
    const auto litRight = std::pair{kArrowResting, kArrowLit};
    const auto resting = std::pair{kArrowResting, kArrowResting};
    using Icons = std::vector<std::string_view>;

    // Of two, right comes round to the very one left would: the right arrow is lit for the step
    // the move first shows in, as the icons swap.
    REQUIRE(pair.selector.selection() == 1);
    std::vector<Quad> quads = press(pair, 0, SelectorInput{.right = true});
    REQUIRE(pair.selector.selection() == 0);
    CHECK(arrowsIn(quads, 0) == litRight);
    CHECK(iconsIn(quads, 0).empty());
    quads = settled();
    CHECK(arrowsIn(quads, 0) == resting);
    CHECK(iconsIn(quads, 0) == Icons{"HUD_LEVITATE", "HUD_INVIS"});
    CHECK(arrowsIn(press(pair, 0, SelectorInput{.left = true}), 0) == litLeft);
    // Left and right at once: Dark Legacy's selector goes left, and so does the arrow.
    settled();
    CHECK(arrowsIn(press(pair, 0, SelectorInput{.left = true, .right = true}), 0) == litLeft);

    // Of one, nothing else comes up and the icon stays, but the press lights its arrow all the
    // same.
    settled();
    quads = press(single, 1, SelectorInput{.right = true});
    CHECK(arrowsIn(quads, 1) == litRight);
    CHECK(iconsIn(quads, 1) == Icons{"HUD_INVIS"});
    CHECK(arrowsIn(settled(), 1) == resting);
    CHECK(arrowsIn(press(single, 1, SelectorInput{.left = true}), 1) == litLeft);

    // Another machine's player's arrow is lit by the way its selection went.
    settled();
    REQUIRE(remote.selector.selection() == 2);
    quads = press(remote, 2, SelectorInput{.right = true});
    REQUIRE(remote.selector.selection() == 0);
    CHECK(arrowsIn(quads, 2) == litRight);
    CHECK(arrowsIn(settled(), 2) == resting);
    CHECK(arrowsIn(press(remote, 2, SelectorInput{.left = true}), 2) == litLeft);
}

TEST_CASE("a new legend item turns its local holder's panel to the quest page, never a remote "
          "seat's",
          "[legends][n64-hud]") {
    // Dark Legacy's lamp, which the N64 has no icon of, 32 by 16 in SELECT.
    const std::array<LentPicture, 1> lend{
        {{.set = test::DarkLegacyLookup::Set::Select, .name = "LAMP", .width = 32, .height = 16}}};
    LoadedLook n64("n64-hud-legend-turn", kWarriorAndWizard, {}, lend);
    // Both hold the scimitar as the look first sees them; seat 1 is another machine's.
    HudSeat here = standing(kWarriorClass, 0, "A");
    here.local = true;
    here.legends = 0x2;
    HudSeat there = standing(kWizardClass, 1, "B");
    there.legends = 0x2;
    HudView view = n64.viewOf({here, there, {}, {}});
    using Icons = std::vector<std::string_view>;
    prepareAt(n64, view, 0);
    std::vector<Quad> quads = n64.panels(view);
    CHECK(iconsIn(quads, 0).empty());
    CHECK(picturesIn(quads, "WAR_P1", 0).size() == 1);
    // The flame scroll found: the panel played here turns to its quest page with the scroll
    // centred, the scimitar before it; the other machine's player's stays as it was.
    view.seats[0].legends = 0x202;
    view.seats[1].legends = 0x202;
    prepareAt(n64, view, 1);
    quads = n64.panels(view);
    CHECK(iconsIn(quads, 0) == Icons{"HUD_SCIMITAR", "HUD_FLAMESCROLL"});
    checkRect(picturesIn(quads, "HUD_FLAMESCROLL", 0).front().area,
              centredAt(0, Layout::kSelected, 28, 28));
    checkRect(picturesIn(quads, "HUD_SCIMITAR", 0).front().area,
              centredAt(0, Layout::kPrevious, 28, 28, 0.6f));
    CHECK(iconsIn(quads, 1).empty());
    CHECK(picturesIn(quads, "WIZ_P1", 1).size() == 1);
    // It stays there until browsed away.
    prepareAt(n64, view, 2);
    CHECK(iconsIn(n64.panels(view), 0) == Icons{"HUD_SCIMITAR", "HUD_FLAMESCROLL"});
    n64.look.browse(0, SelectorInput{.down = true});
    prepareAt(n64, view, 3);
    CHECK(iconsIn(n64.panels(view), 0).empty());
    // The lamp found turns it to Dark Legacy's picture of it, fitted into 28, between the
    // scimitar and the flame scroll.
    view.seats[0].legends = 0x20a;
    prepareAt(n64, view, 4);
    quads = n64.panels(view);
    const std::vector<Quad> lamp = drawsOf(quads, n64.lentPicture("LAMP"), 0);
    REQUIRE(lamp.size() == 1);
    checkRect(lamp.front().area, centredAt(0, Layout::kSelected, 28, 14));
    CHECK(lamp.front().color == Color::white());
    CHECK(iconsIn(quads, 0) == Icons{"HUD_SCIMITAR", "HUD_FLAMESCROLL"});

    // A look that first sees a seat already holding a legend item leaves its page alone.
    LoadedLook later("n64-hud-legend-seen", kWarriorAndWizard);
    HudView seen = later.viewOf({view.seats[0], {}, {}, {}});
    prepareAt(later, seen, 9);
    CHECK(iconsIn(later.panels(seen), 0).empty());

    // One found while the selector is up waits for it to go down: the carousel shows meanwhile,
    // and then the panel turns to the item.
    LoadedLook carousel("n64-hud-legend-under-selector", kWarriorAndWizard);
    Inventory carried;
    carried.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, true};
    Holder holder(carried, 0);
    HudSeat holding = holder.seat(0);
    holding.legends = 0x2;
    HudView up = carousel.viewOf({holding, {}, {}, {}});
    prepareAt(carousel, up, 0);
    holder.open();
    up.seats[0].legends = 0x202;
    prepareAt(carousel, up, 1);
    CHECK(iconsIn(carousel.panels(up), 0) == Icons{"HUD_INVIS"});
    holder.close();
    prepareAt(carousel, up, 2);
    quads = carousel.panels(up);
    REQUIRE(iconsIn(quads, 0) == Icons{"HUD_SCIMITAR", "HUD_FLAMESCROLL"});
    checkRect(picturesIn(quads, "HUD_FLAMESCROLL", 0).front().area,
              centredAt(0, Layout::kSelected, 28, 28));
    // One found under the selector and then taken back by a rewind turns nothing.
    carousel.look.browse(0, SelectorInput{.down = true});
    prepareAt(carousel, up, 3);
    CHECK(iconsIn(carousel.panels(up), 0).empty());
    holder.open();
    up.seats[0].legends = 0x206;
    prepareAt(carousel, up, 5);
    prepareAt(carousel, up, 4);
    holder.close();
    prepareAt(carousel, up, 5);
    CHECK(iconsIn(carousel.panels(up), 0).empty());
}

TEST_CASE("a centred runestone or key entry draws its whole row across the panel",
          "[legends][n64-hud]") {
    // The quest page's entries: its legend items in their realms' order, the thirteenth rune,
    // the runestones and the keys, each while any is held.
    using Kind = LegendsN64QuestEntry::Kind;
    const LegendsN64QuestList listed =
        legendsN64QuestEntries(0x800 | 0x40 | 0x2, StatusBoxPainter::kThirteenthRune | 0x1, 0x100);
    CHECK(std::vector(listed.entries().begin(), listed.entries().end()) ==
          std::vector<LegendsN64QuestEntry>{{.kind = Kind::Legend, .realm = 1},
                                            {.kind = Kind::Legend, .realm = 11},
                                            {.kind = Kind::ThirteenthRune},
                                            {.kind = Kind::Runestones}});
    CHECK(legendsN64QuestEntries(0, 0, 0).entries().empty());

    // STATIC's pictures as Dark Legacy's boxes draw them: the runestones 8 square, the keys
    // and the thirteenth rune 16.
    std::vector<std::string> names{"RUNE13"};
    for (const std::string_view colour : StatusBoxPainter::kRelicColours) {
        for (s32 rune = 1; rune <= 3; ++rune) {
            names.push_back(std::format("SM_RUNE_{}_0{}", colour, rune));
        }
    }
    for (const std::string_view colour : StatusBoxPainter::kRelicColours) {
        names.push_back(std::format("SM_KEY_{}", colour));
    }
    std::vector<LentPicture> lend;
    for (const std::string& name : names) {
        const u32 size = name.starts_with("SM_RUNE") ? 8 : 16;
        lend.push_back({.set = test::DarkLegacyLookup::Set::Static,
                        .name = name,
                        .width = size,
                        .height = size});
    }
    LoadedLook n64("n64-hud-rows", kWarriorAndWizard, {}, lend);
    const auto picture = [&](const std::string& name) { return n64.lentPicture(name); };
    // Runestones 1, 2, 4 and 12, the thirteenth, and the keys of the first, fifth and eighth
    // bosses.
    HudSeat seat = standing(kWarriorClass, 0, "A");
    seat.local = true;
    seat.box.runes = StatusBoxPainter::kThirteenthRune | 0x800 | 0x8 | 0x2 | 0x1;
    seat.box.bossKeys = 0x80 | 0x10 | 0x1;
    HudView view = n64.viewOf({seat, {}, {}, {}});
    prepareAt(n64, view, 0);
    n64.look.browse(0, SelectorInput{.down = true});
    n64.look.browse(0, SelectorInput{.down = true});
    prepareAt(n64, view, 1);
    std::vector<Quad> quads = n64.panels(view);
    // The thirteenth rune centred, fitted into 28; the keys before it shown by the first held,
    // the runestones after it by the first held, each fitted at 0.6.
    const auto only = [&](const std::string& name) {
        const std::vector<Quad> drawn = drawsOf(quads, picture(name), 0);
        REQUIRE(drawn.size() == 1);
        return drawn.front().area;
    };
    checkRect(only("RUNE13"), centredAt(0, Layout::kSelected, 28, 28));
    checkRect(only("SM_KEY_BLU"), centredAt(0, Layout::kPrevious, 28, 28, 0.6f));
    checkRect(only("SM_RUNE_BLU_01"), centredAt(0, Layout::kNext, 28, 28, 0.6f));

    // The rows' pictures drawn in slot 0, in turn, with where they stand.
    const auto row = [&] {
        std::vector<std::pair<std::string, Rect>> drawn;
        for (const Quad& quad : quads) {
            for (const std::string& name : names) {
                if (quad.texture == picture(name) && slotOf(quad) == 0) {
                    drawn.emplace_back(name, quad.area);
                }
            }
        }
        return drawn;
    };
    const auto checkRow = [&](const std::vector<std::pair<std::string, Rect>>& expected) {
        const auto drawn = row();
        REQUIRE(drawn.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(drawn[index].first == expected[index].first);
            checkRect(drawn[index].second, expected[index].second);
        }
    };
    // Right centres the runestones: Dark Legacy's row, a rune every 8 and a unit more after each
    // trio, 99 across from its first place to its last rune's end, centred on the panel's 52
    // and its selection's row 32; nothing stands beside it, and for the step the move first
    // shows in, nothing at all.
    n64.look.browse(0, SelectorInput{.right = true});
    prepareAt(n64, view, 2);
    quads = n64.panels(view);
    CHECK(row().empty());
    prepareAt(n64, view, 3);
    quads = n64.panels(view);
    const f32 runes = 52 - 99.0f / 2;
    const f32 runeTop = kTop + 32 - 4;
    checkRow({{"SM_RUNE_BLU_01", {runes, runeTop, 8, 8}},
              {"SM_RUNE_BLU_02", {runes + 8, runeTop, 8, 8}},
              {"SM_RUNE_RED_01", {runes + 24 + 1, runeTop, 8, 8}},
              {"SM_RUNE_GRE_03", {runes + 88 + 3, runeTop, 8, 8}}});
    // Right again centres the keys: the eight places 12 apart, 100 across, each key held in the
    // GameCube's colour for it, the fifth green and the eighth blue.
    CHECK(StatusBoxPainter::kKeyColours ==
          std::array<std::string_view, 8>{"BLU", "RED", "YEL", "GRE", "GRE", "RED", "YEL", "BLU"});
    n64.look.browse(0, SelectorInput{.right = true});
    prepareAt(n64, view, 4);
    prepareAt(n64, view, 5);
    quads = n64.panels(view);
    const f32 keys = 52 - 100.0f / 2;
    const f32 keyTop = kTop + 32 - 8;
    checkRow({{"SM_KEY_BLU", {keys, keyTop, 16, 16}},
              {"SM_KEY_GRE", {keys + 48, keyTop, 16, 16}},
              {"SM_KEY_BLU", {keys + 84, keyTop, 16, 16}}});
    // Once more and the entries have gone round to the thirteenth rune.
    n64.look.browse(0, SelectorInput{.right = true});
    prepareAt(n64, view, 6);
    prepareAt(n64, view, 7);
    quads = n64.panels(view);
    checkRect(only("RUNE13"), centredAt(0, Layout::kSelected, 28, 28));
}

TEST_CASE("an N64 exit shows its original plaque, a Dark Legacy exit the port's own, and the "
          "tower's none",
          "[legends][n64-hud]") {
    LoadedLook n64("n64-hud-plaques", kWarriorAndWizard);
    // Seat 0 stands on an N64 exit to the castle's first level; seat 1, in red, on a Dark Legacy
    // exit to a castle level; seat 2 on one back to the tower; seat 3, in yellow, on one to a
    // level of the secret realm, which no scroll names. No seat wears its own seat's colour, so a
    // plaque lit by its seat rather than its costume shows.
    LevelRef courtyard;
    courtyard.realmId = 1;
    courtyard.title = "Castle Courtyard";
    LevelRef valley;
    valley.realmId = LevelRef::kSecretRealm;
    valley.title = "Hidden Valley";
    const LevelRef tower = LevelRef::tower();
    std::array<HudSeat, HudView::kSeats> seats{
        standing(kWarriorClass, 3, "A"), standing(kWizardClass, 2, "B"),
        standing(kWarriorClass, 1, "C"), standing(kWizardClass, 0, "D")};
    seats[0].pad = HudPad{.identity = 0x0101, .nativeCode = 0x0101, .destination = nullptr};
    seats[1].pad = HudPad{.identity = kDarkLegacyPadIdentityBase + 4,
                          .nativeCode = std::nullopt,
                          .destination = &courtyard};
    seats[2].pad = HudPad{
        .identity = kDarkLegacyPadIdentityBase, .nativeCode = std::nullopt, .destination = &tower};
    seats[3].pad = HudPad{.identity = kDarkLegacyPadIdentityBase + 7,
                          .nativeCode = std::nullopt,
                          .destination = &valley};
    HudView view = n64.viewOf(seats);
    using Names = std::vector<std::string_view>;
    const Names portMade{"COLOR", "COLOR", "COLOR", "COLOR", "BORDER", "BORDER"};
    CHECK(Layout::kPlaqueRealmInk == Color::rgba(255, 231, 132));

    // Every plaque at transparency `transparency`, drawn last over the panels, in the frame.
    const auto checkPlaques = [&](u8 transparency) {
        CAPTURE(transparency);
        const auto opacity = static_cast<u8>(255 - transparency);
        const Color clear = Color::white().withAlpha(opacity);
        const std::vector<Quad> quads = n64.panels(view);
        const std::vector<Quad> plaques = afterLights(quads);
        // Seat 0's is the N64's own picture of the castle's first level, over its whole panel,
        // and nothing of it stands under the lights.
        CHECK(pictureOrder(plaques, 0) == Names{"LNAME_0101"});
        CHECK(picturesIn(quads, "LNAME_0101").size() == 1);
        const std::vector<Quad> castles = picturesIn(plaques, "LNAME_0101", 0);
        REQUIRE(castles.size() == 1);
        const Quad& castle = castles.front();
        checkRect(castle.area, {0, kTop, 104, 50});
        checkRect(castle.uv, {0, 0, 1, 1});
        CHECK(castle.color == clear);
        CHECK(castle.transform == LegendsN64Frame::transform());
        // Seats 1 and 3 have the port's: the panel's background lit in the costume's bright
        // shade, and its frame. The tower's exit has none.
        for (const usize seat : {usize{1}, usize{3}}) {
            CAPTURE(seat);
            CHECK(pictureOrder(plaques, seat) == portMade);
            const Rect slot{static_cast<f32>(Layout::kSlotX.at(seat)), kTop, 104, 50};
            const Color bright =
                Layout::kBright.at(static_cast<usize>(view.seats.at(seat).box.color))
                    .withAlpha(opacity);
            for (const Quad& quarter : picturesIn(plaques, "COLOR", seat)) {
                CHECK(quarter.color == bright);
                CHECK(quarter.transform == LegendsN64Frame::transform());
            }
            checkRect(covered(picturesIn(plaques, "COLOR", seat)), slot);
            for (const Quad& half : picturesIn(plaques, "BORDER", seat)) {
                CHECK(half.color == clear);
            }
            checkRect(covered(picturesIn(plaques, "BORDER", seat)), slot);
        }
        CHECK(pictureOrder(plaques, 2).empty());
        // The realm's name in font 2 in gold, then the level's in font 8 in white, each centred
        // on its panel's middle through a frame of its own; the secret level's title alone.
        const auto realms = framedLinesOf(plaques, Fixture::kSmallFont);
        REQUIRE(realms.size() == 1);
        // 16 of font 2's 4 and a space of 3, each with the N64's unit: 84 across, centred on
        // panel 1's 156, at row 9.
        checkLineFrame(realms[0].first, {156 - 84.0f / 2, kTop + 9}, 1);
        CHECK(realms[0].second ==
              std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "CASTLE STRONGHOLD"}});
        const auto titles = framedLinesOf(plaques, Fixture::kTitleFont);
        REQUIRE(titles.size() == 2);
        // 15 of font 8's 9 and a space of 6, with their units 157 across, shrunk to the
        // plaque's 96, at row 22.
        checkLineFrame(titles[0].first, {156 - 48, kTop + 22}, 96.0f / 157);
        CHECK(titles[0].second ==
              std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "CASTLE COURTYARD"}});
        // 127 across over panel 3's 364, shrunk alike.
        checkLineFrame(titles[1].first, {364 - 48, kTop + 22}, 96.0f / 127);
        CHECK(titles[1].second ==
              std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "HIDDEN VALLEY"}});
        for (const Quad& quad : plaques) {
            if (Fixture::sheetOf(quad.texture, Fixture::kSmallFont)) {
                CHECK(quad.color == Layout::kPlaqueRealmInk.withAlpha(opacity));
            } else if (Fixture::sheetOf(quad.texture, Fixture::kTitleFont)) {
                CHECK(quad.color == clear);
            }
        }
    };
    // The look's first sight of the seats only sets its clock going: no plaque yet.
    prepareAt(n64, view, 0);
    CHECK(afterLights(n64.panels(view)).empty());
    // A step on, each is made at transparency 0xdf, and six more steps rest them at 0x1f.
    prepareAt(n64, view, 1);
    checkPlaques(0xdf);
    for (s32 step = 2; step <= 7; ++step) {
        prepareAt(n64, view, step);
    }
    checkPlaques(0x1f);

    // Seat 0 steps off: its plaque clouds over and goes as the machine has it, the others
    // staying at rest.
    view.seats[0].pad.reset();
    const auto castleShown = [&] {
        const std::vector<Quad> plaques = afterLights(n64.panels(view));
        CHECK(pictureOrder(plaques, 1) == portMade);
        const auto castle = picturesIn(plaques, "LNAME_0101", 0);
        return castle.empty() ? -1 : s32{castle.front().color.a};
    };
    std::vector<s32> opacities;
    for (s32 step = 8; step <= 12; ++step) {
        prepareAt(n64, view, step);
        opacities.push_back(castleShown());
    }
    CHECK(opacities == std::vector<s32>{255 - 0x5f, 255 - 0x9f, 255 - 0xdf, 255 - 0xdf, -1});

    // An N64 exit whose code has no picture shows nothing however long seat 0 stands on it...
    view.seats[0].pad = HudPad{.identity = 0x0703, .nativeCode = 0x0703, .destination = nullptr};
    for (s32 step = 13; step <= 20; ++step) {
        CAPTURE(step);
        prepareAt(n64, view, step);
        CHECK(pictureOrder(afterLights(n64.panels(view)), 0).empty());
    }
    // ...and gives way at once: the town's second level's picture, from the town's bank, is made
    // the step after the code is let go.
    view.seats[0].pad = HudPad{.identity = 0x0702, .nativeCode = 0x0702, .destination = nullptr};
    prepareAt(n64, view, 21);
    CHECK(pictureOrder(afterLights(n64.panels(view)), 0).empty());
    prepareAt(n64, view, 22);
    std::vector<Quad> plaques = afterLights(n64.panels(view));
    REQUIRE(pictureOrder(plaques, 0) == Names{"LNAME_0702"});
    CHECK(picturesIn(plaques, "LNAME_0702", 0).front().color ==
          Color::white().withAlpha(255 - 0xdf));

    // A rewind stands every plaque where its seat's exit has it at rest.
    prepareAt(n64, view, 5);
    plaques = afterLights(n64.panels(view));
    REQUIRE(pictureOrder(plaques, 0) == Names{"LNAME_0702"});
    CHECK(picturesIn(plaques, "LNAME_0702", 0).front().color ==
          Color::white().withAlpha(255 - 0x1f));
    CHECK(picturesIn(plaques, "BORDER", 1).front().color == Color::white().withAlpha(255 - 0x1f));
    CHECK(pictureOrder(plaques, 2).empty());
}

TEST_CASE("a missing or unreadable level-name bank or title font never stops the N64 look",
          "[legends][n64-hud]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LevelRef courtyard;
    courtyard.realmId = 1;
    courtyard.title = "Castle Courtyard";
    // The plaques a step after seat 0 steps onto the castle's first exit, seat 1 onto the
    // town's first and seat 2 onto one of Dark Legacy's.
    const auto plaquesOf = [&](LegendsN64HudPainter& look) {
        HudView view;
        view.seats = {standing(kWarriorClass, 0, "A"),
                      standing(kWizardClass, 1, "B"),
                      standing(kWarriorClass, 2, "C"),
                      {}};
        view.seats[0].pad =
            HudPad{.identity = 0x0101, .nativeCode = 0x0101, .destination = nullptr};
        view.seats[1].pad =
            HudPad{.identity = 0x0701, .nativeCode = 0x0701, .destination = nullptr};
        view.seats[2].pad = HudPad{.identity = kDarkLegacyPadIdentityBase,
                                   .nativeCode = std::nullopt,
                                   .destination = &courtyard};
        view.strings = &strings;
        test::FakeRenderDevice drawing;
        for (s32 step = 0; step < 2; ++step) {
            view.seconds = secondsAt(step);
            Canvas canvas;
            canvas.begin(drawing, Mat4{1.0f});
            HudFrame frame{canvas, drawing, art};
            look.prepare(frame, view);
            canvas.end();
        }
        Canvas canvas;
        canvas.begin(drawing, Mat4{1.0f});
        HudFrame frame{canvas, drawing, art};
        look.drawStatus(frame, view);
        canvas.end();
        return afterLights(quadsOf(drawing));
    };
    using Names = std::vector<std::string_view>;
    const Names portMade{"COLOR", "COLOR", "COLOR", "COLOR", "BORDER", "BORDER"};
    // The level's name over panel 2's middle, 208 + 52, at row 22, shrunk to the plaque's 96:
    // in font 8, its fixture's 157 across with the N64's units, else in font 5, 15 of its 6 and
    // a space of 5 with theirs, 111 across.
    const auto checkTitle = [](std::span<const Quad> plaques, const test::N64Font& font) {
        const auto titles = framedLinesOf(plaques, font);
        REQUIRE(titles.size() == 1);
        const f32 across = 15 * font.advance() + font.spaceAdvance();
        CHECK(across == (font.file == Fixture::kTitleFont.file ? 157.0f : 111.0f));
        checkLineFrame(titles[0].first, {260 - 48, kTop + 22}, 96 / across);
        CHECK(titles[0].second ==
              std::vector<Line>{{kFirstGlyph.x, kFirstGlyph.y, "CASTLE COURTYARD"}});
    };
    // Loads `look` from `files`; the warnings it wrote. The look must outlive the draws read
    // from it, as they point at its pictures.
    const auto loadFrom = [&](LegendsN64HudPainter& look, const Fixture& files) {
        bool loaded = false;
        auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), art, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        return std::move(run.warnings);
    };

    SECTION("everything there") {
        const Fixture files("n64-hud-plaques-whole");
        LegendsN64HudPainter look;
        CHECK(loadFrom(look, files).empty());
        const std::vector<Quad> plaques = plaquesOf(look);
        CHECK(pictureOrder(plaques, 0) == Names{"LNAME_0101"});
        CHECK(pictureOrder(plaques, 1) == Names{"LNAME_0701"});
        CHECK(pictureOrder(plaques, 2) == portMade);
        checkTitle(plaques, Fixture::kTitleFont);
    }
    SECTION("no level-name banks") {
        const Fixture files("n64-hud-plaques-no-banks");
        REQUIRE(std::filesystem::remove(files.root() / Fixture::kTownPlaqueBank));
        REQUIRE(std::filesystem::remove(files.root() / Fixture::kCastlePlaqueBank));
        LegendsN64HudPainter look;
        CHECK(loadFrom(look, files).empty());
        // The N64's exits show nothing; Dark Legacy's still the port's.
        const std::vector<Quad> plaques = plaquesOf(look);
        CHECK(pictureOrder(plaques, 0).empty());
        CHECK(pictureOrder(plaques, 1).empty());
        CHECK(pictureOrder(plaques, 2) == portMade);
    }
    SECTION("a level-name bank cut short") {
        const Fixture files("n64-hud-plaques-short-bank");
        files.truncate(Fixture::kCastlePlaqueBank);
        LegendsN64HudPainter look;
        // The castle's exits lose theirs, with one warning naming its bank; the town's keep
        // theirs.
        const std::vector<std::string> warnings = loadFrom(look, files);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().find(Fixture::kCastlePlaqueBank) != std::string::npos);
        const std::vector<Quad> plaques = plaquesOf(look);
        CHECK(pictureOrder(plaques, 0).empty());
        CHECK(pictureOrder(plaques, 1) == Names{"LNAME_0701"});
        CHECK(pictureOrder(plaques, 2) == portMade);
    }
    SECTION("no title font") {
        const Fixture files("n64-hud-plaques-no-title");
        REQUIRE(std::filesystem::remove(files.root() / Fixture::kTitleFont.file));
        LegendsN64HudPainter look;
        CHECK(loadFrom(look, files).empty());
        const std::vector<Quad> plaques = plaquesOf(look);
        CHECK(framedLinesOf(plaques, Fixture::kTitleFont).empty());
        checkTitle(plaques, Fixture::kNameFont);
    }
    SECTION("a title font cut short") {
        const Fixture files("n64-hud-plaques-short-title");
        files.truncate(Fixture::kTitleFont.file);
        LegendsN64HudPainter look;
        const std::vector<std::string> warnings = loadFrom(look, files);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().find(Fixture::kTitleFont.file) != std::string::npos);
        const std::vector<Quad> plaques = plaquesOf(look);
        CHECK(framedLinesOf(plaques, Fixture::kTitleFont).empty());
        checkTitle(plaques, Fixture::kNameFont);
    }
}
