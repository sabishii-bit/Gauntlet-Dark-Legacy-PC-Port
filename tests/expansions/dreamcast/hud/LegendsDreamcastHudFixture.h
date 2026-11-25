#pragma once

#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"
#include "SyntheticBytes.h"
#include "TestSupport.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"

namespace gdl::test {

/** One picture of a synthetic Dreamcast texture bank: its name and size, and the mark every
 * texel carries, its low four bits in red and its high four in green, which tells the uploaded
 * picture apart from the others. */
struct DreamcastPicture {
    std::string_view name;
    u16 width = 0;
    u16 height = 0;
    u8 mark = 0;
};

/** A native Dreamcast bank's two files. */
struct DreamcastBank {
    std::vector<u8> objects;  ///< objects.rom: the v1 header, the bitmap records, their names
    std::vector<u8> textures; ///< textures.rom: each bitmap's texels
};

/** `pictures` as a native bank: each an opaque ARGB4444 rectangle of its mark. */
inline DreamcastBank dreamcastTextureBank(std::span<const DreamcastPicture> pictures) {
    constexpr usize kVersionAt = 64;
    constexpr u32 kVersion = 0xF00B0001;
    constexpr usize kNamesAt = 92; ///< where the header says the name table starts
    constexpr usize kHeaderBytes = 104;
    constexpr usize kRecordBytes = 80;
    constexpr usize kRecordFieldBytes = 24; ///< what a record holds before its padding
    constexpr u16 kSignature = 0xFF;
    constexpr u8 kArgb4444 = 2;
    constexpr u8 kRectangle = 9;
    constexpr usize kNameBytes = 36;
    constexpr usize kNameLength = 30;
    constexpr u16 kOpaque = 0xF000;
    constexpr u16 kBlue = 0x5;
    constexpr u32 kNibble = 0xF;
    SyntheticBytes texels;
    std::vector<usize> offsets;
    for (const DreamcastPicture& picture : pictures) {
        offsets.push_back(texels.size());
        const auto texel = static_cast<u16>(kOpaque | ((picture.mark & kNibble) << 8U) |
                                            ((u32{picture.mark} >> 4U) << 4U) | kBlue);
        for (usize at = 0; at < usize{picture.width} * picture.height; ++at) {
            texels.putU16(texel);
        }
    }
    const auto count = static_cast<u32>(pictures.size());
    SyntheticBytes objects;
    objects.putZeros(kVersionAt).putU32(kVersion).putU32(count);
    objects.putZeros(kNamesAt - objects.size());
    objects.putU32(static_cast<u32>(kHeaderBytes + pictures.size() * kRecordBytes));
    objects.putU32(0).putU32(count); // no models, then the bitmaps
    for (usize index = 0; index < pictures.size(); ++index) {
        const DreamcastPicture& picture = pictures[index];
        objects.putU16(kSignature).putU8(0).putU8(0).putU16(picture.width).putU16(picture.height);
        objects.putU32(static_cast<u32>(offsets[index])).putU32(0);
        objects.putU8(kArgb4444).putU8(kRectangle).putU16(0);
        objects.putU32(static_cast<u32>(usize{picture.width} * picture.height * 2));
        objects.putZeros(kRecordBytes - kRecordFieldBytes);
    }
    for (usize index = 0; index < pictures.size(); ++index) {
        const std::string_view name = pictures[index].name;
        objects.putText(name).putZeros(kNameLength - name.size());
        objects.putU16(static_cast<u16>(index)).putZeros(kNameBytes - kNameLength - 2);
    }
    return {objects.bytes(), texels.bytes()};
}

/** A synthetic Dreamcast font: each character of `codes` a cell `columns` wide, side by side
 * along its sheet in the static set, the font `rows` tall. */
struct DreamcastFont {
    std::string_view file;
    std::string_view codes;
    s32 columns = 0;
    s32 rows = 0;
    s32 space = 0; ///< what the look gives its spaces (8c14864c)
    DreamcastPicture sheet;
};

/** `font`'s native file: the header, a glyph record a character and the empty record that ends
 * them. */
inline std::vector<u8> dreamcastFontBytes(const DreamcastFont& font) {
    constexpr usize kRecordBytes = 16;
    SyntheticBytes bytes;
    bytes.putS32(0).putS32(font.rows).putS32(0);
    for (usize index = 0; index < font.codes.size(); ++index) {
        bytes.putS32(static_cast<u8>(font.codes[index])).putS32(font.columns);
        bytes.putS32(static_cast<s32>(index) * font.columns).putS32(0);
    }
    bytes.putZeros(kRecordBytes);
    return bytes.bytes();
}

/** A costume bank of the synthetic extraction: its folder and its panel and banner. */
struct DreamcastCostume {
    std::string_view folder;
    std::array<DreamcastPicture, 2> pictures;
};

/** A synthetic Dreamcast native extraction holding what the Dreamcast look draws from: the
 * static set's pictures, its cards and scrolls among them, and the fonts' sheets, beside one
 * picture the look does not draw; the three fonts; the warrior's costume banks in every colour,
 * the wizard's in blue and the dwarf's in red; and the powerups bank's legend icons and
 * hourglass, beside one picture the look does not draw. */
class LegendsDreamcastHudFixture {
public:
    using Painter = game::LegendsDreamcastHudPainter;

    /** Font 1: the full stop, the slash, the digits, the capitals, the question mark, the small
     * letters and the hyphen. */
    static constexpr DreamcastFont kSmallFont{
        .file = Painter::kSmallFontFile,
        .codes = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ?abcdefghijklmnopqrstuvwxyz-",
        .columns = 6,
        .rows = 8,
        .space = 8,
        .sheet = {.name = "8HI_FONTS5", .width = 512, .height = 8, .mark = 60}};
    /** Font 4: the digits. */
    static constexpr DreamcastFont kScoreFont{
        .file = Painter::kScoreFontFile,
        .codes = "0123456789",
        .columns = 8,
        .rows = 16,
        .space = 9,
        .sheet = {.name = "SCORE", .width = 128, .height = 16, .mark = 61}};
    /** Font 6: the capitals and the small letters. */
    static constexpr DreamcastFont kLargeFont{
        .file = Painter::kLargeFontFile,
        .codes = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
        .columns = 4,
        .rows = 32,
        .space = 16,
        .sheet = {.name = "FONT32", .width = 256, .height = 32, .mark = 62}};
    /** The static set: the look's pictures at the sizes the original's are, the fonts' sheets,
     * and RUNE_COUNT, which the look does not draw. */
    static constexpr std::array<DreamcastPicture, 59> kStaticPictures{{
        {.name = "BK_RUNE_STONE_02", .width = 128, .height = 16, .mark = 10},
        {.name = "S3_WYELO", .width = 128, .height = 16, .mark = 11},
        {.name = "S3_WBLUE", .width = 128, .height = 16, .mark = 12},
        {.name = "S3_WRED", .width = 128, .height = 16, .mark = 13},
        {.name = "S3_WGREEN", .width = 128, .height = 16, .mark = 14},
        {.name = "S4_WYELO", .width = 128, .height = 128, .mark = 15},
        {.name = "S4_WBLUE", .width = 128, .height = 128, .mark = 16},
        {.name = "S4_WRED", .width = 128, .height = 128, .mark = 17},
        {.name = "S4_WGREEN", .width = 128, .height = 128, .mark = 18},
        {.name = "S3_MID", .width = 64, .height = 16, .mark = 53},
        {.name = "S4_MID", .width = 64, .height = 64, .mark = 54},
        {.name = "BTMBK_GOLD", .width = 32, .height = 8, .mark = 19},
        {.name = "BTMBK_HELTH", .width = 64, .height = 8, .mark = 20},
        {.name = "BTMBK_LEVL", .width = 32, .height = 8, .mark = 21},
        {.name = "KEY_ICON", .width = 16, .height = 16, .mark = 22},
        {.name = "POTION_ICON_RED", .width = 16, .height = 16, .mark = 23},
        {.name = "POTION_ICON_BLU", .width = 16, .height = 16, .mark = 24},
        {.name = "POTION_ICON_YEL", .width = 16, .height = 16, .mark = 25},
        {.name = "POTION_ICON_GRE", .width = 16, .height = 16, .mark = 26},
        {.name = "TRBO_GRADNT_GREEN", .width = 128, .height = 32, .mark = 27},
        {.name = "TRBO_GRADNT_YELO", .width = 128, .height = 32, .mark = 28},
        {.name = "TRBO_GRADNT_RED", .width = 128, .height = 32, .mark = 29},
        {.name = "TRBO_FULL", .width = 128, .height = 32, .mark = 30},
        {.name = "TURBO_GLOW", .width = 128, .height = 32, .mark = 31},
        {.name = "SM_RUNE_BLU_01", .width = 8, .height = 8, .mark = 64},
        {.name = "SM_RUNE_BLU_02", .width = 8, .height = 8, .mark = 65},
        {.name = "SM_RUNE_BLU_03", .width = 8, .height = 8, .mark = 66},
        {.name = "SM_RUNE_RED_01", .width = 8, .height = 8, .mark = 67},
        {.name = "SM_RUNE_RED_02", .width = 8, .height = 8, .mark = 68},
        {.name = "SM_RUNE_RED_03", .width = 8, .height = 8, .mark = 69},
        {.name = "SM_RUNE_YEL_01", .width = 8, .height = 8, .mark = 70},
        {.name = "SM_RUNE_YEL_02", .width = 8, .height = 8, .mark = 71},
        {.name = "SM_RUNE_YEL_03", .width = 8, .height = 8, .mark = 72},
        {.name = "SM_RUNE_GRE_01", .width = 8, .height = 8, .mark = 73},
        {.name = "SM_RUNE_GRE_02", .width = 8, .height = 8, .mark = 74},
        {.name = "SM_RUNE_GRE_03", .width = 8, .height = 8, .mark = 75},
        {.name = "SM_KEY_BLU", .width = 16, .height = 16, .mark = 76},
        {.name = "SM_KEY_GRE", .width = 16, .height = 16, .mark = 77},
        {.name = "SM_KEY_RED", .width = 16, .height = 16, .mark = 78},
        {.name = "SM_KEY_YEL", .width = 16, .height = 16, .mark = 79},
        {.name = "RUNE13", .width = 16, .height = 16, .mark = 80},
        {.name = "IT", .width = 64, .height = 64, .mark = 32},
        {.name = "TEXTSCROLL", .width = 256, .height = 256, .mark = 39},
        {.name = "GOLD", .width = 128, .height = 128, .mark = 33},
        {.name = "KEY", .width = 128, .height = 128, .mark = 34},
        {.name = "FOOD", .width = 128, .height = 128, .mark = 35},
        {.name = "POISN_FOOD", .width = 128, .height = 128, .mark = 36},
        {.name = "MAGIC", .width = 128, .height = 128, .mark = 37},
        {.name = "SPECIALS", .width = 128, .height = 128, .mark = 38},
        {.name = "THERMBASE", .width = 128, .height = 128, .mark = 87},
        {.name = "THERMCOL", .width = 128, .height = 128, .mark = 88},
        {.name = "YEL_TEXTSCROLL", .width = 64, .height = 64, .mark = 89},
        {.name = "BLU_TEXTSCROLL", .width = 64, .height = 64, .mark = 90},
        {.name = "RED_TEXTSCROLL", .width = 64, .height = 64, .mark = 91},
        {.name = "GRE_TEXTSCROLL", .width = 64, .height = 64, .mark = 92},
        kSmallFont.sheet,
        kScoreFont.sheet,
        kLargeFont.sheet,
        {.name = "RUNE_COUNT", .width = 32, .height = 32, .mark = 52},
    }};
    /** The picture of the static set the look does not draw: no user of it was found. */
    static constexpr std::string_view kUndrawn = "RUNE_COUNT";
    static constexpr std::string_view kStaticFolder = "static";
    /** The powerups bank: the legend items' icons, the hourglass's glass, sand and five frames of
     * grains, and HOURGLASS, which the look does not draw. */
    static constexpr std::array<DreamcastPicture, 13> kPowerupPictures{{
        {.name = "QUEST_ICON1", .width = 16, .height = 16, .mark = 81},
        {.name = "QUEST_ICON2", .width = 16, .height = 16, .mark = 82},
        {.name = "QUEST_ICON3", .width = 16, .height = 16, .mark = 83},
        {.name = "QUEST_ICON4", .width = 16, .height = 16, .mark = 84},
        {.name = "QUEST_ICON5", .width = 16, .height = 16, .mark = 85},
        {.name = "HOURGLASS", .width = 16, .height = 16, .mark = 86},
        {.name = "TIMER", .width = 128, .height = 128, .mark = 93},
        {.name = "TIMER_SAND", .width = 128, .height = 128, .mark = 94},
        {.name = "SAND_ANIM00", .width = 8, .height = 32, .mark = 95},
        {.name = "SAND_ANIM01", .width = 8, .height = 32, .mark = 96},
        {.name = "SAND_ANIM02", .width = 8, .height = 32, .mark = 97},
        {.name = "SAND_ANIM03", .width = 8, .height = 32, .mark = 98},
        {.name = "SAND_ANIM04", .width = 8, .height = 32, .mark = 99},
    }};
    static constexpr std::string_view kUndrawnPowerup = "HOURGLASS";
    static constexpr std::string_view kPowerupsFolder = Painter::kPowerupsFolder;
    static constexpr std::array<DreamcastCostume, 6> kCostumes{{
        {.folder = "players/war/yel",
         .pictures = {{{.name = "BTMBK_YELO", .width = 128, .height = 128, .mark = 40},
                       {.name = "WAR_NAME", .width = 128, .height = 16, .mark = 41}}}},
        {.folder = "players/war/blu",
         .pictures = {{{.name = "BTMBK_BLUE", .width = 128, .height = 128, .mark = 42},
                       {.name = "WAR_NAME", .width = 128, .height = 16, .mark = 43}}}},
        {.folder = "players/war/red",
         .pictures = {{{.name = "BTMBK_RED", .width = 128, .height = 128, .mark = 44},
                       {.name = "WAR_NAME", .width = 128, .height = 16, .mark = 45}}}},
        {.folder = "players/war/gre",
         .pictures = {{{.name = "BTMBK_GREEN", .width = 128, .height = 128, .mark = 46},
                       {.name = "WAR_NAME", .width = 128, .height = 16, .mark = 47}}}},
        {.folder = "players/wiz/blu",
         .pictures = {{{.name = "BTMBK_BLUE", .width = 128, .height = 128, .mark = 48},
                       {.name = "WIZ_NAME", .width = 128, .height = 16, .mark = 49}}}},
        {.folder = "players/dwf/red",
         .pictures = {{{.name = "BTMBK_RED", .width = 128, .height = 128, .mark = 50},
                       {.name = "DWF_NAME", .width = 128, .height = 16, .mark = 51}}}},
    }};

    explicit LegendsDreamcastHudFixture(std::string_view name) : m_root(scratchDirectory(name)) {
        writeBank(kStaticFolder, kStaticPictures);
        for (const DreamcastCostume& costume : kCostumes) {
            writeBank(costume.folder, costume.pictures);
        }
        writeBank(kPowerupsFolder, kPowerupPictures);
        for (const DreamcastFont& font : {kSmallFont, kScoreFont, kLargeFont}) {
            write(font.file, dreamcastFontBytes(font));
        }
    }

    const std::filesystem::path& root() const { return m_root; }

    /** Writes `bytes` as the extraction's file `name`. */
    void write(std::string_view name, std::span<const u8> bytes) const {
        const std::filesystem::path file = m_root / name;
        std::filesystem::create_directories(file.parent_path());
        writeFile(file, bytes);
    }
    /** Writes a bank of `pictures` as the extraction's bank in `folder`. */
    void writeBank(std::string_view folder, std::span<const DreamcastPicture> pictures) const {
        const DreamcastBank bank = dreamcastTextureBank(pictures);
        write(std::format("{}/objects.rom", folder), bank.objects);
        write(std::format("{}/textures.rom", folder), bank.textures);
    }
    /** Cuts the extraction's file `name` to its first half. */
    void truncate(std::string_view name) const {
        const std::vector<u8> bytes = readFile(m_root / name);
        write(name, std::span{bytes}.first(bytes.size() / 2));
    }

    /** Where picture `texture` comes from: its bank's folder and its name; empty for none of the
     * extraction's. */
    struct Found {
        std::string_view folder;
        std::string_view name;
    };
    static Found pictureOf(const Texture* texture) {
        const auto* image = dynamic_cast<const FakeTexture*>(texture);
        constexpr u8 kChannelStep = 17; ///< a nibble's value in a decoded channel
        if (image == nullptr || image->pixels.size() < 2) {
            return {};
        }
        const auto mark = static_cast<u8>(image->pixels[0] / kChannelStep +
                                          (image->pixels[1] / kChannelStep) * 16U);
        const auto matches = [&](const DreamcastPicture& picture) {
            return picture.mark == mark && picture.width == image->width() &&
                   picture.height == image->height();
        };
        for (const DreamcastPicture& picture : kStaticPictures) {
            if (matches(picture)) {
                return {.folder = kStaticFolder, .name = picture.name};
            }
        }
        for (const DreamcastCostume& costume : kCostumes) {
            for (const DreamcastPicture& picture : costume.pictures) {
                if (matches(picture)) {
                    return {.folder = costume.folder, .name = picture.name};
                }
            }
        }
        for (const DreamcastPicture& picture : kPowerupPictures) {
            if (matches(picture)) {
                return {.folder = kPowerupsFolder, .name = picture.name};
            }
        }
        return {};
    }
    /** Whether `texture` is `font`'s sheet. */
    static bool sheetOf(const Texture* texture, const DreamcastFont& font) {
        const Found found = pictureOf(texture);
        return found.folder == kStaticFolder && found.name == font.sheet.name;
    }

private:
    std::filesystem::path m_root;
};

} // namespace gdl::test
