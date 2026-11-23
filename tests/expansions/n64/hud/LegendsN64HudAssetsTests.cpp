#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"

#include "FakeDarkLegacyArt.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64IntensityAlpha.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/hud/LegendsN64HudIcons.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/n64/hud/LegendsN64Plaque.h"
#include "game/characters/ClassData.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"
#include "game/level/LevelCatalog.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Layout = LegendsN64HudLayout;
using Set = test::DarkLegacyLookup::Set;

/** The HUD bank's pictures the panels draw besides the bottles: the background, lit and empty,
 * the frame, the turbo meter's frame and lights, the heart, the key and the arrow. */
constexpr std::array<std::string_view, 8> kPanelPictures{"COLOR",    "COLORI", "BORDER", "TURBO",
                                                         "TURBOCLR", "HEART1", "KEY1",   "ARROW"};
/** Its advice scrolls: the plain one, then each costume colour's. */
constexpr std::array<std::string_view, 5> kHelpScrolls{
    "TEXTSCROLL", "YEL_TEXTSCROLL", "BLU_TEXTSCROLL", "RED_TEXTSCROLL", "GRE_TEXTSCROLL"};
/** The icons the carousel and the quest page draw: 29 powerups' and 5 legend items'. */
constexpr usize kItemIcons = 34;
/** The thunder hammer's icon, the one larger than the icon square. */
constexpr std::string_view kHammerIcon = "HUD_HAMMER";
constexpr u32 kHammerIconSize = 32;
/** A class's portrait: its light and its shine, after its code. */
constexpr std::array<std::string_view, 2> kPortraitParts{"_P1", "_P2"};
/** The nine classes with N64 portraits, two pictures each. */
constexpr usize kPortraits = 18;
/** The level-name pictures of the N64's five realms' banks. */
constexpr usize kPlaques = 24;
/** The hourglass's glass and sand, and its falling grains' frames, by number after this. */
constexpr std::array<std::string_view, 2> kTimerPictures{"TIMER_P0", "TIMER_P1"};
constexpr std::string_view kGrainsPrefix = "SAND_ANIM";

// The native font header's fields that numfont's case reads (LegendsN64Font): its format and
// texel size codes, its atlas's size, its line, its glyph count and where its atlas starts.
constexpr usize kFormatAt = 1;
constexpr usize kTexelSizeAt = 2;
constexpr usize kAtlasWidthAt = 8;
constexpr usize kAtlasRowsAt = 10;
constexpr usize kLineAt = 12;
constexpr usize kGlyphCountAt = 20;
constexpr usize kFontHeader = 0x24;
constexpr usize kGlyphBytes = 5;
constexpr usize kAtlasAlignment = 16;
constexpr u8 kIntensityAlphaFormat = 3;
constexpr u8 kEightBits = 1;
// numfont.FNT's header: an IA8 atlas of 72x7 texels and a line of 9, whose ten digits reach an
// eighth row.
constexpr u32 kNumberAtlasWidth = 72;
constexpr u32 kNumberAtlasRows = 7;
constexpr u32 kNumberRowsReached = 8;
constexpr s32 kNumberLine = 9;
constexpr usize kDigits = 10;
constexpr u8 kOpaque = 255;

/** The realms Dark Legacy's scrolls name, from one, which the port's plaques write. */
constexpr s32 kNamedRealms = 11;

/** A party of the first four classes, each in its seat's colour: each has its portraits. */
constexpr std::array<HudMember, 4> kParty{{
    {.seat = 0, .classIndex = kWarriorClass, .color = 0},
    {.seat = 1, .classIndex = kValkyrieClass, .color = 1},
    {.seat = 2, .classIndex = kWizardClass, .color = 2},
    {.seat = 3, .classIndex = kArcherClass, .color = 3},
}};

/** The player's own N64 extraction; skips the case without it. */
std::filesystem::path n64Root() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / LegendsN64HudPainter::kHudBank)) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    return root;
}

/** The pictures of the extraction's bank `name`. */
std::vector<formats::LegendsN64Texture> bankOf(const std::filesystem::path& root,
                                               std::string_view name) {
    CAPTURE(name);
    return formats::decodeLegendsN64Textures(readFile(root / name));
}

/** Checks `picture` holds its own pixels, every texel of them. */
void checkWhole(const formats::LegendsN64Texture& picture) {
    CAPTURE(picture.name);
    CHECK_FALSE(picture.external);
    CHECK(picture.width > 0);
    CHECK(picture.height > 0);
    CHECK(picture.image.width == picture.width);
    CHECK(picture.image.height == picture.height);
    CHECK(picture.image.pixels.size() == usize{picture.width} * picture.height * 4);
}

/** Picture `name` of `bank`, checked whole. */
const formats::LegendsN64Texture& wholeIn(std::span<const formats::LegendsN64Texture> bank,
                                          std::string_view name) {
    CAPTURE(name);
    const auto found = std::ranges::find(bank, name, &formats::LegendsN64Texture::name);
    REQUIRE(found != bank.end());
    checkWhole(*found);
    return *found;
}

/** The characters of `text` that `font` has no glyph for, which the text painter skips; a space
 * takes the font's own width. */
std::string missingGlyphs(const BitmapFont& font, std::string_view text) {
    std::string missing;
    for (const char code : text) {
        if (code != ' ' && font.glyph(static_cast<u8>(code)) == nullptr) {
            missing.push_back(code);
        }
    }
    return missing;
}

/** What `art` lends for `lookup`. */
const Texture* lentBy(DarkLegacyHudArt& art, const test::DarkLegacyLookup& lookup) {
    switch (lookup.set) {
    case Set::Static: return art.staticArt(lookup.name);
    case Set::Select: return art.selectArt(lookup.name);
    case Set::Count: return art.countArt(lookup.name);
    case Set::Inventory: return art.inventoryArt(lookup.name);
    }
    return nullptr;
}

} // namespace

TEST_CASE("the N64 HUD bank, portraits, plaques and timer decode whole",
          "[legends][n64-hud][assets]") {
    const std::filesystem::path root = n64Root();

    // The HUD bank holds every picture the look cannot open without: the panels', the bottles,
    // the advice scrolls and every item icon.
    const auto hud = bankOf(root, LegendsN64HudPainter::kHudBank);
    for (const std::string_view name : kPanelPictures) {
        wholeIn(hud, name);
    }
    for (usize kind = 0; kind < StatusBoxPainter::kPotionIcons.size(); ++kind) {
        wholeIn(hud, legendsN64PotionBottle(static_cast<s32>(kind)));
    }
    for (const std::string_view name : kHelpScrolls) {
        wholeIn(hud, name);
    }
    const std::vector<std::string_view> icons = legendsN64ItemIcons();
    CHECK(icons.size() == kItemIcons);
    for (const std::string_view icon : icons) {
        // Each fills the square Dark Legacy's pictures are fitted to in their place, but the
        // hammer's, which is larger.
        const formats::LegendsN64Texture& picture = wholeIn(hud, icon);
        CAPTURE(icon);
        const auto side =
            icon == kHammerIcon ? kHammerIconSize : static_cast<u32>(Layout::kIconSize);
        CHECK(picture.width == side);
        CHECK(picture.height == side);
    }
    // The panel's art fills its 104x50 as the look draws it: the background mirrored both ways,
    // the frame and the turbo meter mirrored across, the meter's lights all their rows tall, and
    // the empty panel whole.
    const auto panelHalf = static_cast<u32>(Layout::kPanelWidth / 2);
    const formats::LegendsN64Texture& background = wholeIn(hud, "COLOR");
    CHECK(background.width == panelHalf);
    CHECK(background.height == static_cast<u32>(Layout::kPanelHeight / 2));
    const formats::LegendsN64Texture& empty = wholeIn(hud, "COLORI");
    CHECK(empty.width == static_cast<u32>(Layout::kPanelWidth));
    CHECK(empty.height == static_cast<u32>(Layout::kPanelHeight));
    const formats::LegendsN64Texture& border = wholeIn(hud, "BORDER");
    CHECK(border.width == panelHalf);
    CHECK(border.height == static_cast<u32>(Layout::kPanelHeight));
    CHECK(wholeIn(hud, "TURBO").width == panelHalf);
    const formats::LegendsN64Texture& lights = wholeIn(hud, "TURBOCLR");
    CHECK(lights.width == panelHalf);
    CHECK(lights.height == static_cast<u32>(Layout::kTurboRows));

    // Every class with portraits has its light and its shine.
    usize portraits = 0;
    for (s32 classIndex = 0; classIndex < kClassCount; ++classIndex) {
        const auto bank = legendsN64ClassBank(classIndex);
        const auto code = legendsN64ClassCode(classIndex);
        REQUIRE(bank.has_value() == code.has_value());
        if (!bank) {
            continue;
        }
        const auto pictures = bankOf(root, std::format("{}/objects.wad", *bank));
        for (const std::string_view part : kPortraitParts) {
            wholeIn(pictures, std::format("{}{}", *code, part));
            ++portraits;
        }
    }
    CHECK(portraits == kPortraits);

    // The level-name pictures, each drawn at its own size over its seat's whole panel.
    std::vector<std::string> plaques;
    for (const std::string_view name : LegendsN64HudPainter::kPlaqueBanks) {
        for (const formats::LegendsN64Texture& picture : bankOf(root, name)) {
            if (!picture.name.starts_with(kLegendsN64PlaquePrefix)) {
                continue;
            }
            checkWhole(picture);
            CHECK(picture.width == static_cast<u32>(Layout::kPanelWidth));
            CHECK(picture.height == static_cast<u32>(Layout::kPanelHeight));
            plaques.push_back(picture.name);
        }
    }
    CHECK(plaques.size() == kPlaques);
    std::ranges::sort(plaques);
    CHECK(std::ranges::adjacent_find(plaques) == plaques.end());

    // The hourglass whole: the look draws all of it or none.
    const auto timer = bankOf(root, LegendsN64HudPainter::kTimerBank);
    for (const std::string_view name : kTimerPictures) {
        wholeIn(timer, name);
    }
    for (usize frame = 0; frame < Layout::kSandFrames; ++frame) {
        wholeIn(timer, std::format("{}{:02}", kGrainsPrefix, frame));
    }
}

TEST_CASE("numfont reads through the intensity-alpha path", "[legends][n64-hud][assets]") {
    const std::filesystem::path root = n64Root();
    const std::vector<u8> bytes = readFile(root / LegendsN64HudPainter::kNumberFontFile);
    REQUIRE(bytes.size() >= kFontHeader);
    // Its atlas is IA8, a byte a texel.
    CHECK(bytes[kFormatAt] == kIntensityAlphaFormat);
    CHECK(bytes[kTexelSizeAt] == kEightBits);
    const u32 width = readU16BE(bytes, kAtlasWidthAt);
    const u32 rows = readU16BE(bytes, kAtlasRowsAt);
    CHECK(width == kNumberAtlasWidth);
    CHECK(rows == kNumberAtlasRows);
    CHECK(readU16BE(bytes, kLineAt) == kNumberLine);
    const usize glyphs = readU32BE(bytes, kGlyphCountAt);
    const usize atlasAt = (kFontHeader + glyphs * kGlyphBytes + kAtlasAlignment - 1) /
                          kAtlasAlignment * kAtlasAlignment;
    REQUIRE(atlasAt + usize{width} * rows <= bytes.size());

    const formats::LegendsN64Font font = formats::LegendsN64Font::parse(bytes);
    CHECK(font.font.height() == kNumberLine);
    CHECK(font.font.glyphCount() == kDigits);
    // Each stored texel is its byte read as IA8, and the row the digits reach below the atlas is
    // clear.
    REQUIRE(font.atlas.width == width);
    REQUIRE(font.atlas.height == kNumberRowsReached);
    usize misread = 0;
    for (u32 y = 0; y < rows; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u8 stored = bytes[atlasAt + usize{y} * width + x];
            if (font.atlas.pixel(x, y) != formats::legendsN64IntensityAlpha8(stored)) {
                ++misread;
            }
        }
    }
    CHECK(misread == 0);
    usize uncleared = 0;
    for (u32 y = rows; y < font.atlas.height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (font.atlas.pixel(x, y) != Color::transparent()) {
                ++uncleared;
            }
        }
    }
    CHECK(uncleared == 0);
    // Every digit is there and shows: some texel of its cell is opaque.
    for (char digit = '0'; digit <= '9'; ++digit) {
        CAPTURE(digit);
        const BitmapGlyph* glyph = font.font.glyph(static_cast<u8>(digit));
        REQUIRE(glyph != nullptr);
        REQUIRE(glyph->columns > 0);
        REQUIRE(glyph->rows > 0);
        bool shows = false;
        for (s32 y = glyph->y; y < glyph->y + glyph->rows; ++y) {
            for (s32 x = glyph->x; x < glyph->x + glyph->columns; ++x) {
                shows = shows ||
                        font.atlas.pixel(static_cast<u32>(x), static_cast<u32>(y)).a == kOpaque;
            }
        }
        CHECK(shows);
    }
}

TEST_CASE("every Dark Legacy level title and realm name draws in the N64 fonts",
          "[legends][n64-hud][assets]") {
    const std::filesystem::path root = n64Root();
    const auto cube = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(cube));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const formats::LegendsN64Font title =
        formats::LegendsN64Font::parse(readFile(root / LegendsN64HudPainter::kTitleFontFile));
    const formats::LegendsN64Font small =
        formats::LegendsN64Font::parse(readFile(root / LegendsN64HudPainter::kSmallFontFile));

    // The port's plaque writes the title of the level an exit leads to as it makes it, in font
    // 8. No plaque names the tower.
    usize titles = 0;
    for (const LevelCatalog::Realm& realm : catalog.realms()) {
        if (realm.id == LevelRef::kTowerRealm) {
            continue;
        }
        for (const std::string& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level.has_value());
            LegendsN64Plaque plaque;
            plaque.step(kDarkLegacyPadIdentityBase, &*level, true);
            REQUIRE(plaque.shown().has_value());
            const std::string& written = plaque.shown()->title;
            CAPTURE(name, written);
            CHECK_FALSE(written.empty());
            CHECK(missingGlyphs(title.font, written) == std::string{});
            ++titles;
        }
    }
    CHECK(titles > 0);
    // And the name of each realm Dark Legacy's scrolls name, in font 2, as the strings spell it.
    for (s32 realm = 1; realm <= kNamedRealms; ++realm) {
        const auto key = darkLegacyRealmNameText(realm);
        REQUIRE(key.has_value());
        REQUIRE(strings.has(*key));
        const std::string_view written = strings.get(*key);
        CAPTURE(realm, written);
        CHECK(missingGlyphs(small.font, written) == std::string{});
    }
}

TEST_CASE("the N64 look opens on the player's own art, and the GameCube data holds every "
          "picture it borrows",
          "[legends][n64-hud][assets]") {
    const std::filesystem::path root = n64Root();
    const auto cube = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::assetOrSkip("SELECT/textures.ngc");
    test::assetOrSkip("INVENTORY/textures.ngc");
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt asked;
    LegendsN64HudPainter look;
    bool loaded = false;
    // Every bank the look reads is there and whole, the optional ones too: the hourglass, the
    // five realms' level names, the title font and the party's portraits.
    const test::LoggedRun opening =
        test::logWhile([&] { loaded = look.load(device, root, asked, kParty); });
    REQUIRE_FALSE(opening.failure);
    CHECK(loaded);
    CHECK(opening.warnings.empty());

    // As it opens it asks Dark Legacy's art for the INVENTORY pictures of the powerups and the
    // SELECT pictures of the legend items it has no icon for, the thirteenth rune, the
    // runestones and the bosses' keys. A count's icon is STATIC's or a level's own, which no
    // level lends here.
    std::vector<test::DarkLegacyLookup> wanted;
    for (const std::string_view name : legendsN64PowerupFallbacks()) {
        wanted.push_back({Set::Inventory, std::string{name}});
    }
    for (const s32 realm : kLegendsN64LegendRealms) {
        if (const auto name = legendsN64LegendFallback(realm)) {
            wanted.push_back({Set::Select, std::string{*name}});
        }
    }
    wanted.push_back({Set::Static, "RUNE13"});
    // Three runestones of each colour, and each colour's key.
    for (const std::string_view colour : StatusBoxPainter::kRelicColours) {
        for (s32 rune = 1; rune <= StatusBoxPainter::kRunesInColour; ++rune) {
            wanted.push_back({Set::Static, std::format("SM_RUNE_{}_{:02}", colour, rune)});
        }
        wanted.push_back({Set::Static, std::format("SM_KEY_{}", colour)});
    }
    std::vector<test::DarkLegacyLookup> borrowed;
    for (const test::DarkLegacyLookup& lookup : asked.lookups) {
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
    REQUIRE(darkLegacy.load(device, cube, nullptr));
    std::vector<std::string> missing;
    const test::LoggedRun lending = test::logWhile([&] {
        for (const test::DarkLegacyLookup& lookup : borrowed) {
            if (lentBy(darkLegacy, lookup) == nullptr) {
                missing.push_back(lookup.name);
            }
        }
    });
    REQUIRE_FALSE(lending.failure);
    CHECK(lending.warnings.empty());
    CHECK(missing == std::vector<std::string>{});
}
