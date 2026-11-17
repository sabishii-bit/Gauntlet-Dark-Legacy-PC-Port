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

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudFixture.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"
#include "expansions/shared/hud/LegendsHudClock.h"
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
using Fixture = test::LegendsDreamcastHudFixture;
using Layout = LegendsDreamcastHudLayout;
using Names = std::vector<std::string_view>;
using test::checkRect;
using test::Quad;
using test::quadsOf;

/** Full health at the level the cases' characters stand at. */
constexpr s32 kMostHealth = 600;
/** A class's name where it has no banner: font32 at half its size. */
constexpr f32 kHalf = 0.5f;
/** Each costume colour's ink, yellow, blue, red and green. */
constexpr Color kYellowInk = Layout::kTextColours[0];
constexpr Color kBlueInk = Layout::kTextColours[1];
constexpr Color kRedInk = Layout::kTextColours[2];

/** The seat whose box a quad stands in, by its left edge: the last whose art starts at or
 * before it. */
usize seatOf(const Quad& quad) {
    usize seat = 0;
    while (seat + 1 < Layout::kSeatX.size() &&
           quad.area.x >= static_cast<f32>(Layout::kSeatX.at(seat + 1) + Layout::kArtLeft)) {
        ++seat;
    }
    return seat;
}

/** Whether a quad is a glyph of one of the fixture's fonts. */
bool isText(const Quad& quad) {
    return Fixture::sheetOf(quad.texture, Fixture::kSmallFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kScoreFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kLargeFont);
}

/** The quads of picture `name` in `seat`, in turn; any seat's when none is named. */
std::vector<Quad> picturesIn(std::span<const Quad> quads, std::string_view name,
                             std::optional<usize> seat = std::nullopt) {
    std::vector<Quad> found;
    for (const Quad& quad : quads) {
        if (!isText(quad) && Fixture::pictureOf(quad.texture).name == name &&
            (!seat || seatOf(quad) == *seat)) {
            found.push_back(quad);
        }
    }
    return found;
}

/** The names of the pictures standing in `seat`, in the order drawn, the glyphs left out. */
Names pictureOrder(std::span<const Quad> quads, usize seat) {
    Names names;
    for (const Quad& quad : quads) {
        const std::string_view name = Fixture::pictureOf(quad.texture).name;
        if (!name.empty() && !isText(quad) && seatOf(quad) == seat) {
            names.push_back(name);
        }
    }
    return names;
}

/** The folder of the bank picture `name` of `seat` came from; empty when it is not drawn. */
std::string_view folderOf(std::span<const Quad> quads, std::string_view name, usize seat) {
    const std::vector<Quad> found = picturesIn(quads, name, seat);
    return found.empty() ? std::string_view{} : Fixture::pictureOf(found.front().texture).folder;
}

/** A line one of the fixture's fonts wrote: its first glyph's place, its characters, read back
 * from where the glyphs were cut from the sheet, a space where the pen skipped one, and its
 * colour. */
struct Line {
    f32 x = 0;
    f32 y = 0;
    std::string text;
    Color color{};
    bool operator==(const Line&) const = default;
};

/** How far a pen writing `text` in `font` at `scale` moves, as TextPainter measures it. */
s32 measure(const test::DreamcastFont& font, std::string_view text, f32 scale = 1) {
    s32 width = 0;
    for (const char code : text) {
        const s32 cell = code == ' '                                  ? font.space
                         : font.codes.find(code) != std::string::npos ? font.columns
                                                                      : 0;
        width += static_cast<s32>(static_cast<f32>(cell) * scale);
    }
    return width;
}

/** The lines `font` wrote at `scale`, in turn: each a run of its glyphs, one after another, along
 * one top. */
std::vector<Line> linesOf(std::span<const Quad> quads, const test::DreamcastFont& font,
                          f32 scale = 1) {
    const auto advance = static_cast<f32>(static_cast<s32>(static_cast<f32>(font.columns) * scale));
    const auto space = static_cast<f32>(static_cast<s32>(static_cast<f32>(font.space) * scale));
    std::vector<Line> lines;
    bool continuing = false;
    f32 previous = 0;
    for (const Quad& quad : quads) {
        if (!Fixture::sheetOf(quad.texture, font)) {
            continuing = false;
            continue;
        }
        // TextPainter samples a glyph from half a texel inside its cell.
        const auto cell =
            static_cast<usize>(std::lround((quad.uv.x * static_cast<f32>(font.sheet.width) - 0.5f) /
                                           static_cast<f32>(font.columns)));
        const char code = font.codes.at(cell);
        const f32 gap = quad.area.x - previous;
        if (continuing && !lines.empty() && lines.back().y == quad.area.y &&
            (gap == advance || gap == advance + space)) {
            if (gap != advance) {
                lines.back().text.push_back(' ');
            }
        } else {
            lines.push_back({quad.area.x, quad.area.y, {}, quad.color});
        }
        lines.back().text.push_back(code);
        previous = quad.area.x;
        continuing = true;
    }
    return lines;
}

/** Where a line written at `scale` from pen point (x, y) lands: half a texel in. */
Line lineAt(s32 x, s32 y, std::string text, Color color, f32 scale = 1) {
    const f32 inset = 0.5f * scale;
    return {static_cast<f32>(x) + inset, static_cast<f32>(y) + inset, std::move(text), color};
}

/** A line in `font` at `scale` centred on `centre`, as TextPainter centres it: half its width,
 * rounded down, before the centre. */
Line centredAt(const test::DreamcastFont& font, s32 centre, s32 y, std::string text, Color color,
               f32 scale = 1) {
    const s32 left = centre - measure(font, text, scale) / 2;
    return lineAt(left, y, std::move(text), color, scale);
}

/** A standing player's seat: class, costume colour and name, `health` of kMostHealth, level 12,
 * 1250 gold, the turbo meter showing nothing. */
HudSeat standing(s32 classIndex, s32 color, std::string name = "A", s32 health = kMostHealth) {
    HudSeat seat;
    seat.box.mode = StatusBoxView::Mode::Status;
    seat.box.active = true;
    seat.box.classIndex = classIndex;
    seat.box.color = color;
    seat.box.name = std::move(name);
    seat.box.level = 12;
    seat.box.gold = 1250;
    seat.box.health = health;
    seat.mostHealth = kMostHealth;
    seat.turbo = 0.0f;
    return seat;
}

/** `seat` fallen, as PartyHud::status shows it: health gone and nothing carried; in the tower,
 * asked to wait there when `prompt`, or else still dying. */
HudSeat fallen(HudSeat seat, bool inTower, bool prompt = false) {
    seat.box.health = 0;
    seat.box.keys = 0;
    seat.box.potions = 0;
    seat.box.potionKind = 0;
    seat.box.inTower = inTower;
    seat.box.towerPrompt = inTower && prompt;
    seat.turbo.reset();
    return seat;
}

/** A player who quit the game from the prompt in seat `seat`: out of the party, its box left
 * empty and its runtime kept, as hudSeatOf shows it. */
struct Departed {
    PlayerRuntime runtime;

    Departed(s32 seat, s32 classIndex, s32 color, std::string name) {
        CharacterSave save;
        save.name = std::move(name);
        save.character = classIndex;
        save.color = color;
        runtime.actor.spawn(seat, save, nullptr, Vec3{0}, 0);
        runtime.departed = true;
    }
    HudSeat seat() const {
        HudSeat shown;
        shown.runtime = &runtime;
        shown.departed = true;
        return shown;
    }
};

/** The Dreamcast look loaded from a synthetic extraction for `party`, with the device it uploads
 * to, Dark Legacy's art it may borrow and the words its boxes write. */
struct LoadedLook {
    Fixture files;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    LegendsDreamcastHudPainter look;

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
    /** The quads the look's boxes draw for `view`, on a canvas of its own. */
    std::vector<Quad> boxes(const HudView& view) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        look.drawStatus(frame, view);
        // The Dreamcast's frame is the canvas's own: nothing is drawn through another.
        CHECK(canvas.localTransform() == Mat4{1.0f});
        canvas.end();
        return quadsOf(device);
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
void prepareAt(LoadedLook& dreamcast, HudView& view, s32 step) {
    view.seconds = secondsAt(step);
    dreamcast.prepare(view);
}

/** A party of a yellow warrior and a blue wizard in the first two seats. */
constexpr std::array<HudMember, 2> kWarriorAndWizard{{
    {.seat = 0, .classIndex = kWarriorClass, .color = 0},
    {.seat = 1, .classIndex = kWizardClass, .color = 1},
}};

} // namespace

TEST_CASE("the Dreamcast look keeps the companion arrows above its boxes",
          "[legends][dreamcast-hud]") {
    LegendsDreamcastHudPainter look;
    CHECK(look.id() == kLegendsDreamcastHudStyle);
    CHECK(look.id() != kDarkLegacyHudStyle);
    // The top of a kind-0 box's strip, where Dark Legacy's bar stands too.
    CHECK(look.statusTop() == 304);
    CHECK(look.statusTop() == static_cast<f32>(StatusBoxPainter::kBarY));

    // A look that never loaded draws nothing.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const StringTable strings;
    HudView view;
    view.seats[0] = standing(kWarriorClass, 0, "ROWENA");
    view.seats[1] = fallen(standing(kWizardClass, 1), true);
    view.strings = &strings;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    HudFrame frame{canvas, device, art};
    look.drawStatus(frame, view);
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("a Dreamcast box draws strip, panel, banner, labels and numbers where the original "
          "does, its counts in the seat's colour",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-box", kWarriorAndWizard);
    // Seat 0, a yellow warrior, carries three keys and two potions, the blue one thrown next,
    // its meter at 45; seat 1, a blue wizard, has little gold and health, its meter empty.
    HudSeat warrior = standing(kWarriorClass, 0);
    warrior.box.keys = 3;
    warrior.box.potions = 2;
    warrior.box.potionKind = 2;
    warrior.turbo = 45.0f;
    HudSeat wizard = standing(kWizardClass, 1, "B", 87);
    wizard.box.level = 3;
    wizard.box.gold = 50;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({warrior, wizard, {}, {}}));

    // Each box stacks its blits as FUN_8c0ba060 and FUN_8c0c08a0 make them: the strip, the
    // costume's panel, the class's banner, the labels, what is carried, then the turbo arch.
    CHECK(pictureOrder(quads, 0) == Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH", "KEY_ICON",
                                          "POTION_ICON_BLU", "TRBO_GRADNT_YELO"});
    CHECK(pictureOrder(quads, 1) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH"});
    // The panels and the banners come from each seat's own costume bank.
    CHECK(folderOf(quads, "BTMBK_YELO", 0) == "players/war/yel");
    CHECK(folderOf(quads, "WAR_NAME", 0) == "players/war/yel");
    CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/wiz/blu");
    CHECK(folderOf(quads, "WIZ_NAME", 1) == "players/wiz/blu");

    // The art stands from 8 left of the column: the strip at 306, the panel's top half at 320,
    // the banner at 338; the labels at (X+34, 323), (X+5, 357) and (X+53, 357).
    const auto only = [&](std::string_view name, usize seat) {
        const std::vector<Quad> found = picturesIn(quads, name, seat);
        REQUIRE(found.size() == 1);
        return found.front();
    };
    for (const usize seat : {usize{0}, usize{1}}) {
        CAPTURE(seat);
        const auto x = static_cast<f32>(dreamcastSeatX(seat));
        checkRect(only("BK_RUNE_STONE_02", seat).area, {x - 8, 306, 128, 16});
        const Quad panel = only(seat == 0 ? "BTMBK_YELO" : "BTMBK_BLUE", seat);
        checkRect(panel.area, {x - 8, 320, 128, 64});
        checkRect(panel.uv, {0, 0, 1, 0.5f});
        checkRect(only(seat == 0 ? "WAR_NAME" : "WIZ_NAME", seat).area, {x - 8, 338, 128, 16});
        checkRect(only("BTMBK_LEVL", seat).area, {x + 34, 323, 32, 8});
        checkRect(only("BTMBK_GOLD", seat).area, {x + 5, 357, 32, 8});
        checkRect(only("BTMBK_HELTH", seat).area, {x + 53, 357, 64, 8});
    }
    // KEY_ICON at (X+1, 323) and the next potion's icon at (X+68, 323).
    checkRect(only("KEY_ICON", 0).area, {9, 323, 16, 16});
    checkRect(only("POTION_ICON_BLU", 0).area, {76, 323, 16, 16});
    for (const Quad& quad : quads) {
        if (!isText(quad)) {
            CHECK(quad.color == Color::white());
        }
    }

    // The level in font 1, white, centred on the box at 330. Gold and health in font 4 at 365,
    // gold from X+3 or, narrower than 32, ending at X+35, health ending at X+92; the key and
    // potion counts from X+18 and X+82 at 323; all of them in the seat's ink (8c14bb98).
    CHECK(kYellowInk == Color::rgba(0xFF, 0xFF, 0x80));
    CHECK(kBlueInk == Color::rgba(0x87, 0xCE, 0xEB));
    CHECK(linesOf(quads, Fixture::kSmallFont) ==
          std::vector<Line>{centredAt(Fixture::kSmallFont, 56, 330, "12", Color::white()),
                            centredAt(Fixture::kSmallFont, 168, 330, "3", Color::white())});
    CHECK(linesOf(quads, Fixture::kScoreFont) ==
          std::vector<Line>{
              lineAt(8 + 3, 365, "1250", kYellowInk), lineAt(8 + 92 - 24, 365, "600", kYellowInk),
              lineAt(8 + 18, 323, "3", kYellowInk), lineAt(8 + 82, 323, "2", kYellowInk),
              lineAt(120 + 35 - 16, 365, "50", kBlueInk),
              lineAt(120 + 92 - 16, 365, "87", kBlueInk)});
    // The gold and health come before what the box carries.
    usize lastNumber = 0;
    usize firstKey = quads.size();
    for (usize index = 0; index < quads.size(); ++index) {
        if (Fixture::sheetOf(quads[index].texture, Fixture::kScoreFont) &&
            quads[index].area.y > 360) {
            lastNumber = seatOf(quads[index]) == 0 ? index : lastNumber;
        }
        if (Fixture::pictureOf(quads[index].texture).name == "KEY_ICON") {
            firstKey = std::min(firstKey, index);
        }
    }
    CHECK(lastNumber < firstKey);

    // A lone player in the third seat stands in that seat's column.
    const std::vector<Quad> lone =
        dreamcast.boxes(dreamcast.viewOf({HudSeat{}, {}, standing(kWarriorClass, 0), {}}));
    REQUIRE_FALSE(lone.empty());
    for (const Quad& quad : lone) {
        CHECK(seatOf(quad) == 2);
    }
    const std::vector<Quad> strip = picturesIn(lone, "BK_RUNE_STONE_02");
    REQUIRE(strip.size() == 1);
    checkRect(strip.front().area, {288, 306, 128, 16});
}

TEST_CASE("keys and potions show only while carried", "[legends][dreamcast-hud]") {
    const std::array<HudMember, 3> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
    }};
    LoadedLook dreamcast("dreamcast-hud-carried", party);
    // Seat 0 carries nothing; seat 1 a key alone; seat 2 a green potion alone.
    const HudSeat nothing = standing(kWarriorClass, 0);
    HudSeat keyed = standing(kWizardClass, 1);
    keyed.box.keys = 1;
    HudSeat dosed = standing(kWarriorClass, 2);
    dosed.box.potions = 1;
    dosed.box.potionKind = 4;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({nothing, keyed, dosed, {}}));
    // FUN_8c0c16e0 shows the key and the potion, and their counts, only while some are carried.
    CHECK(picturesIn(quads, "KEY_ICON", 0).empty());
    CHECK(picturesIn(quads, "KEY_ICON", 1).size() == 1);
    CHECK(picturesIn(quads, "KEY_ICON", 2).empty());
    for (const std::string_view potion :
         {"POTION_ICON_RED", "POTION_ICON_BLU", "POTION_ICON_YEL"}) {
        CAPTURE(potion);
        CHECK(picturesIn(quads, potion).empty());
    }
    const std::vector<Quad> green = picturesIn(quads, "POTION_ICON_GRE");
    REQUIRE(green.size() == 1);
    CHECK(seatOf(green.front()) == 2);
    checkRect(green.front().area, {296 + 68, 323, 16, 16});
    // Only the key's count and the potion's are written in their row.
    std::vector<Line> counts;
    for (const Line& line : linesOf(quads, Fixture::kScoreFont)) {
        if (line.y == 323.5f) {
            counts.push_back(line);
        }
    }
    CHECK(counts == std::vector<Line>{lineAt(120 + 18, 323, "1", kBlueInk),
                                      lineAt(296 + 82, 323, "1", kRedInk)});
}

TEST_CASE("a class with no Dreamcast banner shows Dark Legacy's name in its place, and Sumner "
          "the N64 HUD's word",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 4> party{{
        {.seat = 0, .classIndex = kOgreClass, .color = 0},
        {.seat = 1, .classIndex = kSumnerClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
        {.seat = 3, .classIndex = kUnicornClass, .color = 3},
    }};
    LoadedLook dreamcast("dreamcast-hud-bannerless", party);
    const std::vector<Quad> quads =
        dreamcast.boxes(dreamcast.viewOf({standing(kOgreClass, 0), standing(kSumnerClass, 1),
                                          standing(kWarriorClass, 2), standing(kUnicornClass, 3)}));
    // The Dreamcast has no ogre, Sumner or unicorn. Each takes the warrior's panel of its colour,
    // which every class's of that colour is, and in the banner's place Dark Legacy's lower-case
    // name of its class, or for Sumner the N64 HUD's word, in font32 at half its size, white,
    // centred on the box at 338.
    CHECK(linesOf(quads, Fixture::kLargeFont, kHalf) ==
          std::vector<Line>{
              centredAt(Fixture::kLargeFont, 56, 338, "Ogre", Color::white(), kHalf),
              centredAt(Fixture::kLargeFont, 168, 338, "Sumner", Color::white(), kHalf),
              centredAt(Fixture::kLargeFont, 456, 338, "Unicorn", Color::white(), kHalf)});
    CHECK(pictureOrder(quads, 0) ==
          Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH"});
    CHECK(folderOf(quads, "BTMBK_YELO", 0) == "players/war/yel");
    CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/war/blu");
    CHECK(folderOf(quads, "BTMBK_GREEN", 3) == "players/war/gre");
    // The warrior keeps its own banner.
    CHECK(folderOf(quads, "WAR_NAME", 2) == "players/war/red");
    for (const usize seat : {usize{0}, usize{1}, usize{3}}) {
        CAPTURE(seat);
        CHECK(picturesIn(quads, "WAR_NAME", seat).empty());
    }
}

TEST_CASE("an empty seat draws nothing; a departed seat keeps the kind-0 panel",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-departed", kWarriorAndWizard);
    const Departed gone(1, kWizardClass, 2, "MERLIN");
    const std::vector<Quad> quads =
        dreamcast.boxes(dreamcast.viewOf({HudSeat{}, gone.seat(), {}, {}}));
    // FUN_8c0bbee0 rebuilds a quitting player's box as kind 0, in its costume's colour, and the
    // box is never hidden after: the strip at 304 and the panel's top half at 320, and no text.
    CHECK(pictureOrder(quads, 1) == Names{"S3_WRED", "S4_WRED"});
    REQUIRE(quads.size() == 2);
    checkRect(quads[0].area, {112, 304, 128, 16});
    checkRect(quads[1].area, {112, 320, 128, 64});
    checkRect(quads[1].uv, {0, 0, 1, 0.5f});
    // The seats nobody joined draw nothing: FUN_8c0ba060 makes all their blits hidden.
    for (const Quad& quad : quads) {
        CHECK(seatOf(quad) == 1);
    }
    CHECK(dreamcast.boxes(dreamcast.viewOf({HudSeat{}, {}, {}, {}})).empty());
}

TEST_CASE("a fallen player's Dreamcast box says IN TOWER on the kind-0 panel",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 3> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 1},
        {.seat = 1, .classIndex = kWizardClass, .color = 3},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
    }};
    LoadedLook dreamcast("dreamcast-hud-fallen", party);
    // Seat 0, blue, waits in the tower; seat 1, green, is asked whether to wait there; seat 2,
    // red, is still dying.
    const HudSeat waiting = fallen(standing(kWarriorClass, 1), true);
    const HudSeat asked = fallen(standing(kWizardClass, 3), true, true);
    const HudSeat dying = fallen(standing(kWarriorClass, 2), false);
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({waiting, asked, dying, {}}));

    // FUN_8c0bbf80 and FUN_8c0bc040 rebuild a fallen player's box as kind 0: the coloured strip
    // at 304 and panel at 320, with no banner, labels, numbers, keys or arch.
    CHECK(pictureOrder(quads, 0) == Names{"S3_WBLUE", "S4_WBLUE"});
    CHECK(pictureOrder(quads, 1) == Names{"S3_WGREEN", "S4_WGREEN"});
    const std::vector<Quad> strip = picturesIn(quads, "S3_WBLUE");
    const std::vector<Quad> panel = picturesIn(quads, "S4_WBLUE");
    REQUIRE(strip.size() == 1);
    REQUIRE(panel.size() == 1);
    checkRect(strip.front().area, {0, 304, 128, 16});
    checkRect(panel.front().area, {0, 320, 128, 64});
    checkRect(panel.front().uv, {0, 0, 1, 0.5f});
    // FUN_8c0c16e0 writes IN TOWER (8c1256b4) in font 1, in the seat's ink, centred on the box
    // at 344, for the player waiting in the tower alone. The dying player's level follows.
    CHECK(linesOf(quads, Fixture::kSmallFont) ==
          std::vector<Line>{centredAt(Fixture::kSmallFont, 56, 344, "IN TOWER", kBlueInk),
                            centredAt(Fixture::kSmallFont, 344, 330, "12", Color::white())});

    // Dying, the box is still the panel with its banner, labels, level and health, nothing
    // carried and no arch; and FUN_8c0c0f40 writes no gold for a player whose death plays out
    // (8c0c1298..8c0c12a4 leave it for a seat in state 5 or 6).
    CHECK(pictureOrder(quads, 2) == Names{"BK_RUNE_STONE_02", "BTMBK_RED", "WAR_NAME", "BTMBK_LEVL",
                                          "BTMBK_GOLD", "BTMBK_HELTH"});
    CHECK(linesOf(quads, Fixture::kScoreFont) ==
          std::vector<Line>{lineAt(296 + 92 - 8, 365, "0", kRedInk)});
    CHECK(linesOf(quads, Fixture::kLargeFont, kHalf).empty());
}

TEST_CASE("a missing or truncated class bank falls back to another class's panel",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 2> party{{
        {.seat = 0, .classIndex = kDwarfClass, .color = 2},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
    }};
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    /** What a red dwarf's box shows beside a blue wizard's: where its panel came from, whether
     * its banner stands, and the names written in the banners' place. */
    struct Shown {
        std::string_view panel;
        bool banner = false;
        std::vector<Line> names;
    };
    const auto shownBy = [&](LegendsDreamcastHudPainter& look) {
        test::FakeRenderDevice drawing;
        Canvas canvas;
        canvas.begin(drawing, Mat4{1.0f});
        HudFrame frame{canvas, drawing, art};
        HudView view;
        view.seats[0] = standing(kDwarfClass, 2);
        view.seats[1] = standing(kWizardClass, 1);
        view.strings = &strings;
        look.drawStatus(frame, view);
        canvas.end();
        const std::vector<Quad> quads = quadsOf(drawing);
        // The boxes stand either way, and the wizard's is its own.
        CHECK(picturesIn(quads, "BK_RUNE_STONE_02").size() == 2);
        CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/wiz/blu");
        CHECK(folderOf(quads, "WIZ_NAME", 1) == "players/wiz/blu");
        return Shown{.panel = folderOf(quads, "BTMBK_RED", 0),
                     .banner = !picturesIn(quads, "DWF_NAME", 0).empty(),
                     .names = linesOf(quads, Fixture::kLargeFont, kHalf)};
    };
    const std::vector<Line> dwarf{
        centredAt(Fixture::kLargeFont, 56, 338, "Dwarf", Color::white(), kHalf)};

    SECTION("everything there") {
        const Fixture files("dreamcast-hud-class-whole");
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, party));
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/dwf/red");
        CHECK(shown.banner);
        CHECK(shown.names.empty());
    }
    SECTION("a class bank missing") {
        const Fixture files("dreamcast-hud-class-missing");
        REQUIRE(std::filesystem::remove(files.root() / "players/dwf/red/objects.rom"));
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        CHECK(run.warnings.empty());
        // The warrior's panel of its colour stands in, and the class is named in the banner's
        // place.
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/war/red");
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
    SECTION("a class bank cut short") {
        const Fixture files("dreamcast-hud-class-short");
        files.truncate("players/dwf/red/objects.rom");
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == 1);
        CHECK(run.warnings.front().find("players/dwf/red") != std::string::npos);
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/war/red");
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
    SECTION("the warrior's bank gone too") {
        const Fixture files("dreamcast-hud-class-gone");
        REQUIRE(std::filesystem::remove(files.root() / "players/dwf/red/objects.rom"));
        REQUIRE(std::filesystem::remove(files.root() / "players/war/red/textures.rom"));
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        CHECK(run.warnings.empty());
        // The box stands without a panel, its class still named.
        const Shown shown = shownBy(look);
        CHECK(shown.panel.empty());
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
}

TEST_CASE("the typed name is never drawn", "[legends][dreamcast-hud]") {
    const std::array<HudMember, 4> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
        {.seat = 3, .classIndex = kOgreClass, .color = 3},
    }};
    LoadedLook dreamcast("dreamcast-hud-typed-names", party);
    // The names are spelt in the fonts' own letters, so that they would be read back were they
    // written.
    const std::array<std::string, 4> typed{"ROWENA", "MERLIN", "THYRA", "GROG"};
    HudSeat standingWarrior = standing(kWarriorClass, 0, typed[0]);
    standingWarrior.turbo = 100.0f;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf(
        {standingWarrior, fallen(standing(kWizardClass, 1, typed[1]), true),
         fallen(standing(kWarriorClass, 2, typed[2]), false), standing(kOgreClass, 3, typed[3])}));
    REQUIRE_FALSE(quads.empty());
    std::vector<Line> written = linesOf(quads, Fixture::kSmallFont);
    for (const Line& line : linesOf(quads, Fixture::kScoreFont)) {
        written.push_back(line);
    }
    for (const Line& line : linesOf(quads, Fixture::kLargeFont, kHalf)) {
        written.push_back(line);
    }
    REQUIRE_FALSE(written.empty());
    for (const Line& line : written) {
        for (const std::string& name : typed) {
            CAPTURE(line.text, name);
            CHECK(toLowerAscii(line.text).find(toLowerAscii(name)) == std::string::npos);
        }
    }
    // Nor is any of it in Dark Legacy's small capitals, as its own boxes write it.
    for (const Quad& quad : quads) {
        CHECK(quad.texture != &dreamcast.art.smallCapsSheet());
    }
}

TEST_CASE("the turbo arch stands over a standing player's box, cropped from its centre, and a "
          "full one glows",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-turbo", kWarriorAndWizard);
    HudSeat rising = standing(kWarriorClass, 0);
    rising.turbo = 45.0f;
    HudSeat full = standing(kWizardClass, 1);
    full.turbo = 100.0f;
    HudView view = dreamcast.viewOf({rising, full, {}, {}});
    prepareAt(dreamcast, view, 0);
    const std::vector<Quad> quads = dreamcast.boxes(view);

    // Seat 0's yellow arch, cropped about texel 56 to a half-width of 31 (FUN_8c0c13e0).
    const std::vector<Quad> arch = picturesIn(quads, "TRBO_GRADNT_YELO", 0);
    REQUIRE(arch.size() == 1);
    checkRect(arch.front().area, {8 - 8 + 57 - 31, 306, 62, 32});
    checkRect(arch.front().uv, {25.0f / 128, 0, 62.0f / 128, 1});
    CHECK(picturesIn(quads, "TURBO_GLOW", 0).empty());
    // Seat 1's is whole, its glow over it.
    CHECK(pictureOrder(quads, 1) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH", "TRBO_FULL",
                                          "TURBO_GLOW"});
    const std::vector<Quad> whole = picturesIn(quads, "TRBO_FULL", 1);
    REQUIRE(whole.size() == 1);
    checkRect(whole.front().area, {112, 306, 128, 32});
    checkRect(whole.front().uv, {0, 0, 1, 1});

    // The glow pulses a step at a time while the arch is full, its transparency 0, 12, 24, ...;
    // while the meter is not full it is hidden and its pulse waits, and a rewind starts it over.
    const auto glowAt = [&](s32 step) -> std::optional<u8> {
        prepareAt(dreamcast, view, step);
        const std::vector<Quad> glow = picturesIn(dreamcast.boxes(view), "TURBO_GLOW", 1);
        if (glow.empty()) {
            return std::nullopt;
        }
        REQUIRE(glow.size() == 1);
        checkRect(glow.front().area, {112, 306, 128, 32});
        return glow.front().color.a;
    };
    const auto opaqueAt = [](s32 transparency) {
        return std::optional<u8>{static_cast<u8>(255 - transparency)};
    };
    CHECK(glowAt(0) == opaqueAt(0));
    CHECK(glowAt(1) == opaqueAt(12));
    CHECK(glowAt(2) == opaqueAt(24));
    view.seats[1].turbo = 50.0f;
    for (s32 step = 3; step < 6; ++step) {
        CAPTURE(step);
        CHECK_FALSE(glowAt(step));
    }
    view.seats[1].turbo = 100.0f;
    CHECK(glowAt(6) == opaqueAt(36));
    CHECK(glowAt(2) == opaqueAt(0));
    CHECK(glowAt(3) == opaqueAt(12));
}

TEST_CASE("the Dreamcast look takes every picture it borrows as it opens",
          "[legends][dreamcast-hud]") {
    using Set = test::DarkLegacyLookup::Set;
    const Fixture files("dreamcast-hud-borrowed");
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    // Dark Legacy's art stands ready to lend what a look might ask of it.
    std::vector<const Texture*> lent;
    for (const Set set : {Set::Static, Set::Select, Set::Count, Set::Inventory}) {
        for (const std::string_view name :
             {"RUNE13", "SM_KEY_BLU", "LAMP", "GOLD", "SHP_XRAY_GLASSES"}) {
            lent.push_back(&art.give(set, name));
        }
    }
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LegendsDreamcastHudPainter look;
    REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
    // The boxes draw the Dreamcast's own art alone: they borrow nothing of Dark Legacy's.
    CHECK(art.lookups.empty());
    // It uploads what the boxes draw of the static set, with the fonts' sheets, and the party's
    // panels and banners: nothing else it reads.
    CHECK(device.texturesCreated == Fixture::kStaticPictures.size() - 1 + 4);
    const u32 uploaded = device.texturesCreated;

    // Every seat state and every part of the HUD, over a few steps: nothing is looked up or
    // uploaded while drawing.
    HudSeat carrying = standing(kWarriorClass, 0, "A", 250);
    carrying.box.keys = 2;
    carrying.box.potions = 1;
    carrying.box.potionKind = 3;
    carrying.turbo = 100.0f;
    const Departed gone(3, kWizardClass, 3, "D");
    HudView view;
    view.seats = {carrying, fallen(standing(kWizardClass, 1), true, true),
                  fallen(standing(kWarriorClass, 0), true), gone.seat()};
    view.strings = &strings;
    for (s32 step = 0; step < 3; ++step) {
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
    }
    CHECK(art.lookups.empty());
    CHECK(device.texturesCreated == uploaded);
    REQUIRE_FALSE(device.draws.empty());
    for (const test::RecordedDraw& draw : device.draws) {
        CHECK(std::ranges::find(lent, draw.texture) == lent.end());
        CHECK(Fixture::pictureOf(draw.texture).name != Fixture::kUndrawn);
    }
}

TEST_CASE("the Dreamcast look draws the parts it has no art of its own for in Dark Legacy's look",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-lent-look", kWarriorAndWizard);
    const HudView view = dreamcast.viewOf({standing(kWarriorClass, 0), {}, {}, {}});
    Canvas canvas;
    canvas.begin(dreamcast.device, Mat4{1.0f});
    HudFrame frame{canvas, dreamcast.device, dreamcast.art};
    dreamcast.look.prepare(frame, view);
    dreamcast.look.drawStatus(frame, view);
    dreamcast.look.drawRuneMeter(frame, view);
    dreamcast.look.drawHourglass(frame, view);
    dreamcast.look.drawBossMeters(frame, view);
    dreamcast.look.drawSelectors(frame, view);
    dreamcast.look.drawHelp(frame, view);
    canvas.end();
    // The boxes are the Dreamcast's own; the runestone finder, the hourglass, the boss meters,
    // the selector labels and the help are Dark Legacy's, prepared as its look prepares them.
    const test::FakeHudRecord& darkLegacy = dreamcast.art.record;
    CHECK(darkLegacy.count(kDarkLegacyHudStyle, test::FakeHudPart::Status) == 0);
    for (const test::FakeHudPart part :
         {test::FakeHudPart::Prepare, test::FakeHudPart::RuneMeter, test::FakeHudPart::Hourglass,
          test::FakeHudPart::BossMeters, test::FakeHudPart::Selectors, test::FakeHudPart::Help}) {
        CAPTURE(static_cast<s32>(part));
        CHECK(darkLegacy.count(kDarkLegacyHudStyle, part) == 1);
    }
    CHECK(darkLegacy.calls.size() == 6);
}

TEST_CASE("a missing static set or font stops the Dreamcast look, and one that cannot be read "
          "says which",
          "[legends][dreamcast-hud]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    SECTION("everything there") {
        const Fixture files("dreamcast-hud-load-whole");
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
    }
    SECTION("the static set or a font missing") {
        for (const std::string_view name : LegendsDreamcastHudPainter::kRequiredFiles) {
            CAPTURE(name);
            const Fixture files("dreamcast-hud-load-missing");
            REQUIRE(std::filesystem::remove(files.root() / name));
            LegendsDreamcastHudPainter look;
            const u32 before = device.texturesCreated;
            CHECK_FALSE(look.load(device, files.root(), art, kWarriorAndWizard));
            CHECK(device.texturesCreated == before);
        }
    }
    SECTION("the static set or a font cut short") {
        // What cannot be read stops the look, naming the file, for the opener's one warning. A
        // font cut short has lost the empty record that ends its glyphs.
        for (const std::string_view name : LegendsDreamcastHudPainter::kRequiredFiles) {
            CAPTURE(name);
            const Fixture files("dreamcast-hud-load-short");
            files.truncate(name);
            LegendsDreamcastHudPainter look;
            try {
                (void)look.load(device, files.root(), art, kWarriorAndWizard);
                FAIL("a cut-short file was read");
            } catch (const FormatError& error) {
                CHECK(std::string_view{error.what()}.find(name) != std::string_view::npos);
            }
        }
    }
    SECTION("a static set without a picture the boxes draw") {
        for (const test::DreamcastPicture& left : Fixture::kStaticPictures) {
            if (left.name == Fixture::kUndrawn) {
                continue;
            }
            CAPTURE(left.name);
            const Fixture files("dreamcast-hud-load-incomplete");
            std::vector<test::DreamcastPicture> pictures;
            for (const test::DreamcastPicture& picture : Fixture::kStaticPictures) {
                if (picture.name != left.name) {
                    pictures.push_back(picture);
                }
            }
            files.writeBank(Fixture::kStaticFolder, pictures);
            LegendsDreamcastHudPainter look;
            try {
                (void)look.load(device, files.root(), art, kWarriorAndWizard);
                FAIL("a static set without a picture was taken");
            } catch (const FormatError& error) {
                CHECK(std::string_view{error.what()}.find(left.name) != std::string_view::npos);
            }
        }
    }
    SECTION("without the picture it does not draw") {
        const Fixture files("dreamcast-hud-load-undrawn");
        std::vector<test::DreamcastPicture> pictures;
        for (const test::DreamcastPicture& picture : Fixture::kStaticPictures) {
            if (picture.name != Fixture::kUndrawn) {
                pictures.push_back(picture);
            }
        }
        files.writeBank(Fixture::kStaticFolder, pictures);
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
    }
}
