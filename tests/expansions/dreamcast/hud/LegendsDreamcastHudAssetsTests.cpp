#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "formats/FontFile.h"
#include "game/characters/ClassData.h"
#include "game/config/GameConfig.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Layout = LegendsDreamcastHudLayout;
using Painter = LegendsDreamcastHudPainter;
using Pictures = std::vector<formats::LegendsMapTexture>;
using Set = test::DarkLegacyLookup::Set;

/** The static set's bitmap slots. */
constexpr usize kStaticSlots = 105;
/** The pictures of the boxes' art the look takes from the static set by these names; the rest
 * are named by the layout and by Dark Legacy's boxes. */
constexpr std::array<std::string_view, 13> kStaticArt{
    "BK_RUNE_STONE_02", "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH", "KEY_ICON", "RUNE13", "IT",
    "TEXTSCROLL",       "THERMBASE",  "THERMCOL",   "8HI_FONTS5",  "SCORE",    "FONT32"};
/** A kind-0 box's strip and panel, by costume colour after these. */
constexpr std::array<std::string_view, 2> kPlainBoxArt{"S3_W", "S4_W"};
/** Each class's panel in each colour and its banner, twelve classes in four colours. */
constexpr usize kClassPictures = 96;
/** The powerups bank's hourglass: its glass and sand, and its falling grains' frames, by
 * number after this. */
constexpr std::array<std::string_view, 2> kTimerArt{"TIMER", "TIMER_SAND"};
constexpr std::string_view kGrainsPrefix = "SAND_ANIM";

// 1ST_READ.BIN, loaded at 8c010000: 8c148624 points to each font handle's file's name and
// 8c14864c holds the width its spaces take, a word a handle.
constexpr u32 kExecutableBase = 0x8c010000;
constexpr u32 kFontNames = 0x8c148624;
constexpr u32 kFontSpaces = 0x8c14864c;
constexpr usize kWordBytes = 4;
constexpr usize kLongestName = 32;

/** A font the boxes write in: its handle, its file and the width the executable gives its
 * spaces. */
struct FontHandle {
    s32 handle = 0;
    std::string_view file;
    s32 space = 0;
};
constexpr std::array<FontHandle, 3> kFonts{{
    {.handle = 1, .file = Painter::kSmallFontFile, .space = 8},
    {.handle = 4, .file = Painter::kScoreFontFile, .space = 9},
    {.handle = 6, .file = Painter::kLargeFontFile, .space = 16},
}};

/** The player's own Dreamcast extraction's native folder; skips the case without it. */
std::filesystem::path dreamcastRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / Painter::kStaticObjects)) {
        SKIP("Native Dreamcast Legends extraction is not installed");
    }
    return root;
}

/** The pictures of the extraction's bank in `folder`. */
Pictures bankOf(const std::filesystem::path& root, std::string_view folder) {
    CAPTURE(folder);
    const std::filesystem::path bank = root / folder;
    return formats::decodeLegendsDreamcastTextures(readFile(bank / "objects.rom"),
                                                   readFile(bank / "textures.rom"));
}

/** Checks `picture` holds its own pixels, every texel of them. */
void checkWhole(const formats::LegendsMapTexture& picture) {
    CAPTURE(picture.name);
    CHECK_FALSE(picture.external);
    CHECK(picture.width > 0);
    CHECK(picture.height > 0);
    CHECK(picture.image.width == picture.width);
    CHECK(picture.image.height == picture.height);
    CHECK(picture.image.pixels.size() == usize{picture.width} * picture.height * 4);
}

/** Picture `name` of `bank`, checked whole. */
const formats::LegendsMapTexture& wholeIn(std::span<const formats::LegendsMapTexture> bank,
                                          std::string_view name) {
    CAPTURE(name);
    const auto found = std::ranges::find(bank, name, &formats::LegendsMapTexture::name);
    REQUIRE(found != bank.end());
    checkWhole(*found);
    return *found;
}

/** Every picture the look takes from the static set. */
std::vector<std::string> staticPictures() {
    std::vector<std::string> names(kStaticArt.begin(), kStaticArt.end());
    for (const std::string_view box : kPlainBoxArt) {
        for (const std::string_view colour : Layout::kPanelColours) {
            names.push_back(std::format("{}{}", box, colour));
        }
    }
    names.insert(names.end(), StatusBoxPainter::kPotionIcons.begin(),
                 StatusBoxPainter::kPotionIcons.end());
    for (const LegendsDreamcastTurboZone& zone : Layout::kTurboZones) {
        names.emplace_back(zone.art);
    }
    names.emplace_back(Layout::kTurboFullArt);
    names.emplace_back(Layout::kTurboGlowArt);
    // Three runestones of each colour, and each colour's key.
    for (const std::string_view colour : StatusBoxPainter::kRelicColours) {
        for (s32 rune = 1; rune <= StatusBoxPainter::kRunesInColour; ++rune) {
            names.push_back(std::format("SM_RUNE_{}_{:02}", colour, rune));
        }
        names.push_back(std::format("SM_KEY_{}", colour));
    }
    names.insert(names.end(), Layout::kHelpScrolls.begin(), Layout::kHelpScrolls.end());
    for (const LegendsDreamcastCardKind& card : Layout::kCards) {
        if (!card.face.empty()) {
            names.emplace_back(card.face);
        }
    }
    return names;
}

/** The costume bank of class `code` in costume colour `color`, players/<class>/<colour>. */
std::string costumeBank(std::string_view code, s32 color) {
    return std::format("players/{}/{}", toLowerAscii(code),
                       Layout::kColourFolders.at(static_cast<usize>(color)));
}

/** The quads of `quads` whose texture, as the fake device made it, is `picture`'s pixels. */
std::vector<test::Quad> showing(std::span<const test::Quad> quads,
                                const formats::LegendsMapTexture& picture) {
    std::vector<test::Quad> found;
    for (const test::Quad& quad : quads) {
        if (test::showsPixels(quad, picture.image)) {
            found.push_back(quad);
        }
    }
    return found;
}

/** Whether one of `quads` stands with its top left at (left, top). */
bool standsAt(std::span<const test::Quad> quads, s32 left, s32 top) {
    return std::ranges::any_of(quads, [&](const test::Quad& quad) {
        return quad.area.x == static_cast<f32>(left) && quad.area.y == static_cast<f32>(top);
    });
}

/** The executable's text at `address`, up to its ending nought. */
std::string textAt(std::span<const u8> image, u32 address) {
    REQUIRE(address >= kExecutableBase);
    usize offset = address - kExecutableBase;
    std::string text;
    while (offset < image.size() && image[offset] != 0 && text.size() < kLongestName) {
        text.push_back(static_cast<char>(image[offset++]));
    }
    REQUIRE(offset < image.size());
    REQUIRE(image[offset] == 0);
    return text;
}

/** How far the pen went for the first space of `text`, which `font` wrote at `scale` as the
 * quads `glyphs` of its characters, every one before the space a glyph of the font's. */
s32 spaceWritten(std::span<const test::Quad> glyphs, std::string_view text,
                 const formats::FontFile& font, f32 scale) {
    CAPTURE(text);
    const usize space = text.find(' ');
    REQUIRE(space != std::string_view::npos);
    REQUIRE(space > 0);
    REQUIRE(glyphs.size() == text.size() - static_cast<usize>(std::ranges::count(text, ' ')));
    const auto before = std::ranges::find(font.glyphs, static_cast<s32>(text[space - 1]),
                                          &formats::FontGlyph::code);
    REQUIRE(before != font.glyphs.end());
    const f32 gap = glyphs[space].area.x - glyphs[space - 1].area.x;
    return static_cast<s32>(std::lround(gap)) -
           static_cast<s32>(static_cast<f32>(before->width) * scale);
}

/** A standing player's seat of class `classIndex` in costume colour `color`. */
HudSeat standing(s32 classIndex, s32 color) {
    HudSeat seat;
    seat.box.mode = StatusBoxView::Mode::Status;
    seat.box.active = true;
    seat.box.classIndex = classIndex;
    seat.box.color = color;
    seat.box.level = 12;
    seat.box.gold = 1250;
    seat.box.health = 600;
    seat.mostHealth = 600;
    seat.turbo = 0.0f;
    return seat;
}

/** A player of class `classIndex` in costume colour `color`, fallen to the tower. */
HudSeat inTower(s32 classIndex, s32 color) {
    HudSeat seat = standing(classIndex, color);
    seat.box.health = 0;
    seat.box.inTower = true;
    seat.turbo.reset();
    return seat;
}

/** The Dreamcast look opened on the player's own extraction for `party`: the device it uploads
 * to, Dark Legacy's art as a test lends it, which has nothing to lend, the words its boxes
 * write, and what the log warned of as it opened. */
struct OpenedLook {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    Painter look;
    bool loaded = false;
    std::vector<std::string> warnings;

    OpenedLook(const std::filesystem::path& root, std::span<const HudMember> party) {
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const test::LoggedRun run =
            test::logWhile([&] { loaded = look.load(device, root, art, party); });
        REQUIRE_FALSE(run.failure);
        warnings = run.warnings;
    }

    using Slot = void (Painter::*)(HudFrame&, const HudView&);
    /** The quads the look's `slot` draws for `view`, with these words. */
    std::vector<test::Quad> drawn(HudView view, Slot slot) {
        view.strings = &strings;
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        (look.*slot)(frame, view);
        canvas.end();
        return test::quadsOf(device);
    }
};

} // namespace

TEST_CASE("the Dreamcast static, class and powerup banks hold every texture the look names",
          "[legends][dreamcast-hud][assets]") {
    const std::filesystem::path root = dreamcastRoot();

    // Every slot of the static set is whole, and it holds all the boxes' art the look cannot
    // open without: the kind-0 boxes' S4_W panels among it, and the fonts' sheets.
    const Pictures statics = bankOf(root, "static");
    CHECK(statics.size() == kStaticSlots);
    for (const formats::LegendsMapTexture& picture : statics) {
        checkWhole(picture);
    }
    for (const std::string& name : staticPictures()) {
        wholeIn(statics, name);
    }

    // Each of the twelve classes with banks has its panel in every costume colour, and its
    // banner.
    usize classPictures = 0;
    for (s32 classIndex = 0; classIndex < kClassCount; ++classIndex) {
        const auto code = dreamcastClassCode(classIndex);
        if (!code) {
            continue;
        }
        for (usize color = 0; color < Layout::kColourFolders.size(); ++color) {
            const Pictures bank = bankOf(root, costumeBank(*code, static_cast<s32>(color)));
            wholeIn(bank, std::format("BTMBK_{}", Layout::kPanelColours.at(color)));
            wholeIn(bank, std::format("{}_NAME", *code));
            classPictures += 2;
        }
    }
    CHECK(classPictures == kClassPictures);

    // The powerups bank holds the Dreamcast's five legend items' icons and the whole hourglass.
    const Pictures powerups = bankOf(root, Painter::kPowerupsFolder);
    for (const LegendsDreamcastLegend& legend : Layout::kLegends) {
        if (!legend.icon.empty()) {
            wholeIn(powerups, legend.icon);
        }
    }
    for (const std::string_view name : kTimerArt) {
        wholeIn(powerups, name);
    }
    for (usize frame = 0; frame < Layout::kSandFrames; ++frame) {
        wholeIn(powerups, std::format("{}{:02}", kGrainsPrefix, frame));
    }
}

TEST_CASE("the Dreamcast fonts load with the executable's space widths",
          "[legends][dreamcast-hud][assets]") {
    const std::filesystem::path root = dreamcastRoot();
    const std::filesystem::path executable = root.parent_path() / "disc/1ST_READ.BIN";
    if (!std::filesystem::is_regular_file(executable)) {
        SKIP("optional Dreamcast executable is not installed");
    }
    const std::vector<u8> image = readFile(executable);
    for (const FontHandle& font : kFonts) {
        CAPTURE(font.handle, font.file);
        const usize entry = static_cast<usize>(font.handle) * kWordBytes;
        const u32 name = readU32LE(image, kFontNames - kExecutableBase + entry);
        CHECK(toLowerAscii(textAt(image, name)) ==
              std::filesystem::path{font.file}.stem().string());
        CHECK(readS32LE(image, kFontSpaces - kExecutableBase + entry) == font.space);
        // The look refuses a font whose glyph table does not reach its end, and none of these
        // has a glyph of its own for the space: each writes its spaces at the table's width.
        const formats::FontFile parsed = formats::FontFile::parse(readFile(root / font.file));
        CHECK(parsed.closed);
        CHECK_FALSE(parsed.glyphs.empty());
        CHECK(std::ranges::none_of(parsed.glyphs, [](const formats::FontGlyph& glyph) {
            return glyph.code == static_cast<s32>(' ');
        }));
    }

    // The look writes with those widths: IN TOWER in font 1, and in font 6, at the banner's half
    // size, the name of a class with no banner. No class's name has a space in it, so the ogre
    // is given one of two words here. The score font writes digits alone.
    constexpr std::array<HudMember, 2> kParty{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kOgreClass, .color = 1},
    }};
    OpenedLook dreamcast(root, kParty);
    REQUIRE(dreamcast.loaded);
    constexpr std::string_view kTwoWords = "Ogre Ogre";
    const auto words = test::scratchDirectory("dreamcast-hud-assets-words");
    writeTextFile(words / "en.json",
                  std::format(R"({{"help.player_class_lc.{}": "{}"}})", kOgreClass + 1, kTwoWords));
    REQUIRE(dreamcast.strings.layer(words));
    const Pictures statics = bankOf(root, "static");
    const formats::FontFile small =
        formats::FontFile::parse(readFile(root / Painter::kSmallFontFile));
    const formats::FontFile large =
        formats::FontFile::parse(readFile(root / Painter::kLargeFontFile));

    HudView tower;
    tower.seats[0] = inTower(kWarriorClass, 0);
    const std::vector<test::Quad> inTowerGlyphs =
        showing(dreamcast.drawn(tower, &Painter::drawStatus), wholeIn(statics, "8HI_FONTS5"));
    CHECK(spaceWritten(inTowerGlyphs, dreamcast.strings.get(Layout::kInTowerText), small, 1) ==
          kFonts[0].space);

    HudView named;
    named.seats[1] = standing(kOgreClass, 1);
    const std::vector<test::Quad> nameGlyphs =
        showing(dreamcast.drawn(named, &Painter::drawStatus), wholeIn(statics, "FONT32"));
    CHECK(spaceWritten(nameGlyphs, kTwoWords, large, Layout::kBannerTextScale) ==
          static_cast<s32>(static_cast<f32>(kFonts[2].space) * Layout::kBannerTextScale));
}

TEST_CASE("the Dreamcast look opens on the player's own art and draws each box from its class's "
          "bank",
          "[legends][dreamcast-hud][assets]") {
    const std::filesystem::path root = dreamcastRoot();
    // A warrior, a wizard and a tigress, each with banks of its own, and an ogre, which has none:
    // the warrior's panel stands in for its own, and its name for its banner.
    constexpr std::array<HudMember, 4> kParty{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
        {.seat = 2, .classIndex = kTigressClass, .color = 2},
        {.seat = 3, .classIndex = kOgreClass, .color = 3},
    }};
    OpenedLook dreamcast(root, kParty);
    CHECK(dreamcast.loaded);
    // Every bank it reads is there and whole, the optional ones too.
    CHECK(dreamcast.warnings.empty());

    // The warrior holds the scimitar, the first of the Dreamcast's own legend items.
    HudView view;
    for (usize seat = 0; seat < kParty.size(); ++seat) {
        view.seats.at(seat) = standing(kParty.at(seat).classIndex, kParty.at(seat).color);
    }
    view.seats[0].legends = static_cast<u16>(1U << 1U);
    const std::vector<test::Quad> boxes = dreamcast.drawn(view, &Painter::drawStatus);
    const Pictures statics = bankOf(root, "static");
    for (usize seat = 0; seat < kParty.size(); ++seat) {
        const HudMember& member = kParty.at(seat);
        CAPTURE(seat, member.classIndex);
        const s32 left = dreamcastSeatX(seat) + Layout::kArtLeft;
        const auto code = dreamcastClassCode(member.classIndex);
        const Pictures bank = bankOf(root, costumeBank(code.value_or("WAR"), member.color));
        const formats::LegendsMapTexture& panel =
            wholeIn(bank, std::format("BTMBK_{}",
                                      Layout::kPanelColours.at(static_cast<usize>(member.color))));
        CHECK(standsAt(showing(boxes, panel), left, Layout::kPanelTop));
        if (code) {
            const formats::LegendsMapTexture& banner = wholeIn(bank, std::format("{}_NAME", *code));
            CHECK(standsAt(showing(boxes, banner), left, Layout::kBannerTop));
        }
    }
    // The ogre's name stands in its own box, in font 6.
    const std::vector<test::Quad> named = showing(boxes, wholeIn(statics, "FONT32"));
    CHECK_FALSE(named.empty());
    CHECK(std::ranges::all_of(named, [](const test::Quad& quad) {
        return quad.area.x >= static_cast<f32>(dreamcastSeatX(3) + Layout::kArtLeft);
    }));
    const Pictures powerups = bankOf(root, Painter::kPowerupsFolder);
    CHECK(showing(boxes, wholeIn(powerups, "QUEST_ICON1")).size() == 1);

    // The hourglass is the Dreamcast's own.
    HudView timed;
    timed.hourglass = ChallengeHud::Look{.elapsed = 0.5f, .fallingFrame = 2};
    const std::vector<test::Quad> hourglass = dreamcast.drawn(timed, &Painter::drawHourglass);
    CHECK(standsAt(showing(hourglass, wholeIn(powerups, "TIMER")),
                   static_cast<s32>(ChallengeHud::kFrameArea.x),
                   static_cast<s32>(ChallengeHud::kFrameArea.y)));
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Hourglass) == 0);
}

TEST_CASE("the Dreamcast look opens on the player's own art, and the GameCube data holds every "
          "picture it borrows",
          "[legends][dreamcast-hud][assets]") {
    const std::filesystem::path root = dreamcastRoot();
    const auto cube = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    constexpr std::array<HudMember, 4> kParty{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kValkyrieClass, .color = 1},
        {.seat = 2, .classIndex = kWizardClass, .color = 2},
        {.seat = 3, .classIndex = kArcherClass, .color = 3},
    }};
    OpenedLook dreamcast(root, kParty);
    CHECK(dreamcast.loaded);
    CHECK(dreamcast.warnings.empty());

    // As it opens it asks Dark Legacy's art for the STATIC cards of the pickups the Dreamcast
    // has no card for, and the SELECT pictures of the legend items it has no icon for. A count's
    // icon is STATIC's or a level's own, which no level lends here.
    std::vector<test::DarkLegacyLookup> wanted;
    for (const LegendsDreamcastCardKind& card : Layout::kCards) {
        if (card.face.empty()) {
            wanted.push_back({Set::Static, std::string{card.card}});
        }
    }
    for (const LegendsDreamcastLegend& legend : Layout::kLegends) {
        if (legend.icon.empty()) {
            wanted.push_back({Set::Select, std::string{legend.picture}});
        }
    }
    std::vector<test::DarkLegacyLookup> borrowed;
    for (const test::DarkLegacyLookup& lookup : dreamcast.art.lookups) {
        if (lookup.set != Set::Count) {
            borrowed.push_back(lookup);
        }
    }
    for (const test::DarkLegacyLookup& picture : wanted) {
        CAPTURE(picture.name);
        CHECK(std::ranges::find(borrowed, picture) != borrowed.end());
    }
    CHECK(borrowed.size() == wanted.size());

    // The GameCube's own art lends every one of them.
    DarkLegacyHudPainter darkLegacy;
    REQUIRE(darkLegacy.load(dreamcast.device, cube, nullptr));
    std::vector<std::string> missing;
    const test::LoggedRun lending = test::logWhile([&] {
        for (const test::DarkLegacyLookup& lookup : borrowed) {
            if (test::lentBy(darkLegacy, lookup) == nullptr) {
                missing.push_back(lookup.name);
            }
        }
    });
    REQUIRE_FALSE(lending.failure);
    CHECK(lending.warnings.empty());
    CAPTURE(missing);
    CHECK(missing.empty());
}
