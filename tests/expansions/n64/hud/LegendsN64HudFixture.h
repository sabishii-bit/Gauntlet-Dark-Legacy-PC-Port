#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"
#include "SyntheticBytes.h"
#include "TestSupport.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"

namespace gdl::test {

/** One picture of a synthetic N64 texture bank: its name and size, and the mark every texel's
 * red carries, which tells the uploaded picture apart from the others. */
struct N64Picture {
    std::string_view name;
    u16 width = 0;
    u16 height = 0;
    u8 mark = 0;
    bool intensity = false; ///< stored as I8, as COLOR and COLORI are, rather than as RGBA32
};

/** A native N64 object WAD holding `pictures`: its TXHD records, their NAME and TEXR bytes, and
 * one TXLD load list, which ends at once, for them all. */
inline std::vector<u8> n64TextureBank(std::span<const N64Picture> pictures) {
    constexpr usize kHeaderBytes = 8;
    constexpr usize kRecordBytes = 24;
    constexpr usize kAlignment = 8; ///< TXHD sizes its storage in eight-byte units
    constexpr u32 kEndList = 0xDF000000;
    constexpr u8 kRgbaFormat = 0;
    constexpr u8 kThirtyTwoBits = 3;
    constexpr u8 kIntensityFormat = 4;
    constexpr u8 kEightBits = 1;
    constexpr std::array<u8, 3> kRgbaRest{0x40, 0x80, 0xff}; // a texel's green, blue and alpha
    const auto aligned = [](usize size) {
        return (size + kAlignment - 1) / kAlignment * kAlignment;
    };
    std::string names;
    std::vector<u8> texels;
    std::vector<usize> nameAt;
    std::vector<usize> texelAt;
    for (const N64Picture& picture : pictures) {
        nameAt.push_back(names.size());
        names.append(picture.name);
        names.push_back('\0');
        texelAt.push_back(texels.size());
        for (usize texel = 0; texel < usize{picture.width} * picture.height; ++texel) {
            texels.push_back(picture.mark);
            if (!picture.intensity) {
                texels.insert(texels.end(), kRgbaRest.begin(), kRgbaRest.end());
            }
        }
        texels.resize(aligned(texels.size()));
    }
    const usize nameStart = kHeaderBytes + pictures.size() * kRecordBytes;
    const usize loader = aligned(nameStart + names.size());
    const usize texelStart = loader + kAlignment;
    const usize directory = texelStart + texels.size();
    SyntheticBytes bytes;
    bytes.putU32BE(static_cast<u32>(directory)).putU32BE(4);
    for (usize index = 0; index < pictures.size(); ++index) {
        const N64Picture& picture = pictures[index];
        const usize end = index + 1 < pictures.size() ? texelAt[index + 1] : texels.size();
        bytes.putU32BE(static_cast<u32>(texelAt[index])).putU32BE(static_cast<u32>(loader));
        bytes.putU32BE(0).putU16BE(picture.width).putU16BE(picture.height);
        bytes.putU16BE(static_cast<u16>((end - texelAt[index]) / kAlignment));
        bytes.putU8(picture.intensity ? kIntensityFormat : kRgbaFormat);
        bytes.putU8(picture.intensity ? kEightBits : kThirtyTwoBits);
        bytes.putU16BE(static_cast<u16>(nameAt[index])).putU16BE(0);
    }
    bytes.putText(names).putZeros(loader - bytes.size());
    bytes.putU32BE(kEndList).putU32BE(0);
    bytes.putBytes(texels);
    const std::array<std::pair<std::string_view, std::pair<usize, usize>>, 4> sections{{
        {"TXHD", {kHeaderBytes, pictures.size()}},
        {"NAME", {nameStart, names.size()}},
        {"TXLD", {loader, kAlignment}},
        {"TEXR", {texelStart, texels.size()}},
    }};
    for (const auto& [tag, extent] : sections) {
        bytes.putText(tag).putU32BE(static_cast<u32>(extent.first));
        bytes.putU32BE(static_cast<u32>(extent.second));
    }
    return bytes.bytes();
}

/** A synthetic N64 font: each character of `codes` a solid cell `columns` wide and `rows` tall,
 * side by side along an atlas `atlasRows` tall, which the cells may reach past as numfont's do.
 * It folds lower case onto its capitals, as the HUD's fonts do. */
struct N64Font {
    std::string_view file;
    std::string_view codes;
    u8 columns = 0;
    u8 rows = 0;
    u16 atlasRows = 0;
    u16 lineHeight = 0;
    u16 space = 0;
    bool intensityAlpha = false; ///< an IA8 atlas, as numfont's, rather than I4

    u32 sheetWidth() const { return static_cast<u32>(codes.size()) * columns; }
    u32 sheetHeight() const { return std::max<u32>(atlasRows, rows); }
};

/** `font`'s native file: the 0x24-byte header, the glyph directory, the atlas. */
inline std::vector<u8> n64FontBytes(const N64Font& font) {
    constexpr usize kHeaderBytes = 0x24;
    constexpr usize kAtlasAlignment = 16;
    constexpr u32 kFoldsLowerCase = 1;
    constexpr u8 kIntensityAlphaFormat = 3;
    constexpr u8 kIntensityFormat = 4;
    constexpr u8 kFourBits = 0;
    constexpr u8 kEightBits = 1;
    constexpr u8 kSolid = 0xff;
    SyntheticBytes bytes;
    bytes.putU8(0).putU8(font.intensityAlpha ? kIntensityAlphaFormat : kIntensityFormat);
    bytes.putU8(font.intensityAlpha ? kEightBits : kFourBits).putZeros(5);
    bytes.putU16BE(static_cast<u16>(font.sheetWidth())).putU16BE(font.atlasRows);
    bytes.putU16BE(font.lineHeight).putU16BE(font.space);
    bytes.putU32BE(kFoldsLowerCase).putU32BE(static_cast<u32>(font.codes.size()));
    bytes.putZeros(kHeaderBytes - bytes.size());
    for (usize index = 0; index < font.codes.size(); ++index) {
        bytes.putU8(static_cast<u8>(font.codes[index]));
        bytes.putU8(static_cast<u8>(index * font.columns)).putU8(0);
        bytes.putU8(font.columns).putU8(font.rows);
    }
    bytes.putZeros((kAtlasAlignment - bytes.size() % kAtlasAlignment) % kAtlasAlignment);
    const usize texels = usize{font.sheetWidth()} * font.atlasRows;
    bytes.putBytes(std::vector<u8>(font.intensityAlpha ? texels : (texels + 1) / 2, kSolid));
    return bytes.bytes();
}

/** A synthetic N64 extraction holding what the N64 look draws from: the HUD bank's pictures,
 * the three fonts, the warrior's and the wizard's portraits, the wizard's light as tall as
 * WIZ_P1, and the hourglass's timer bank. */
class LegendsN64HudFixture {
public:
    /** The HUD bank's pictures the look draws: the panels', the advice scrolls, and the
     * carousel's and the quest page's icons, the thunder hammer's bigger than the rest. */
    static constexpr std::array<N64Picture, 51> kHudPictures{{
        {.name = "COLOR", .width = 52, .height = 25, .mark = 10, .intensity = true},
        {.name = "COLORI", .width = 104, .height = 50, .mark = 11, .intensity = true},
        {.name = "BORDER", .width = 52, .height = 50, .mark = 12},
        {.name = "TURBO", .width = 52, .height = 15, .mark = 13},
        {.name = "TURBOCLR", .width = 52, .height = 15, .mark = 14},
        {.name = "HEART1", .width = 11, .height = 10, .mark = 15},
        {.name = "KEY1", .width = 12, .height = 15, .mark = 16},
        {.name = "ARROW", .width = 4, .height = 7, .mark = 17},
        {.name = "POTIONB", .width = 11, .height = 14, .mark = 18},
        {.name = "POTIONG", .width = 11, .height = 14, .mark = 19},
        {.name = "POTIONR", .width = 11, .height = 14, .mark = 20},
        {.name = "POTIONY", .width = 11, .height = 14, .mark = 21},
        {.name = "TEXTSCROLL", .width = 64, .height = 32, .mark = 22},
        {.name = "YEL_TEXTSCROLL", .width = 64, .height = 32, .mark = 23},
        {.name = "BLU_TEXTSCROLL", .width = 64, .height = 32, .mark = 24},
        {.name = "RED_TEXTSCROLL", .width = 64, .height = 32, .mark = 25},
        {.name = "GRE_TEXTSCROLL", .width = 64, .height = 32, .mark = 26},
        {.name = "HUD_3WAY", .width = 28, .height = 28, .mark = 50},
        {.name = "HUD_5WAY", .width = 28, .height = 28, .mark = 51},
        {.name = "HUD_SUPERSHOT", .width = 28, .height = 28, .mark = 52},
        {.name = "HUD_REFLSHOT", .width = 28, .height = 28, .mark = 53},
        {.name = "HUD_HAMMER", .width = 32, .height = 32, .mark = 54},
        {.name = "HUD_RAPIDFIRE", .width = 28, .height = 28, .mark = 55},
        {.name = "HUD_FIREAMLT", .width = 28, .height = 28, .mark = 56},
        {.name = "HUD_ELECAMLT", .width = 28, .height = 28, .mark = 57},
        {.name = "HUD_LITEAMLT", .width = 28, .height = 28, .mark = 58},
        {.name = "HUD_ACIDAMLT", .width = 28, .height = 28, .mark = 59},
        {.name = "HUD_INVULNG", .width = 28, .height = 28, .mark = 60},
        {.name = "HUD_INVULN", .width = 28, .height = 28, .mark = 61},
        {.name = "HUD_HALO", .width = 28, .height = 28, .mark = 62},
        {.name = "HUD_REFLSHIELD", .width = 28, .height = 28, .mark = 63},
        {.name = "HUD_FIRESHIELD", .width = 28, .height = 28, .mark = 64},
        {.name = "HUD_ELECSHIELD", .width = 28, .height = 28, .mark = 65},
        {.name = "HUD_SPEEDBOOST", .width = 28, .height = 28, .mark = 66},
        {.name = "HUD_LEVITATE", .width = 28, .height = 28, .mark = 67},
        {.name = "HUD_INVIS", .width = 28, .height = 28, .mark = 68},
        {.name = "HUD_TIMESTOP", .width = 28, .height = 28, .mark = 69},
        {.name = "HUD_FIREBRETH", .width = 28, .height = 28, .mark = 70},
        {.name = "HUD_ACIDBRETH", .width = 28, .height = 28, .mark = 71},
        {.name = "HUD_ELECBRETH", .width = 28, .height = 28, .mark = 72},
        {.name = "HUD_PHOENIX", .width = 28, .height = 28, .mark = 73},
        {.name = "HUD_GROWTH", .width = 28, .height = 28, .mark = 74},
        {.name = "HUD_SHRINK", .width = 28, .height = 28, .mark = 75},
        {.name = "HUD_SHORN", .width = 28, .height = 28, .mark = 76},
        {.name = "HUD_SMASK", .width = 28, .height = 28, .mark = 77},
        {.name = "HUD_SGAUNTR", .width = 28, .height = 28, .mark = 78},
        {.name = "HUD_SCIMITAR", .width = 28, .height = 28, .mark = 79},
        {.name = "HUD_ICEAXE", .width = 28, .height = 28, .mark = 80},
        {.name = "HUD_SAVIOUR", .width = 28, .height = 28, .mark = 81},
        {.name = "HUD_FLAMESCROLL", .width = 28, .height = 28, .mark = 82},
        {.name = "HUD_JAVELIN", .width = 28, .height = 28, .mark = 83},
    }};
    /** screens/timer's hourglass: the glass, its sand and the five frames of falling grains. */
    static constexpr std::array<N64Picture, 7> kTimerPictures{{
        {.name = "TIMER_P0", .width = 82, .height = 64, .mark = 40},
        {.name = "TIMER_P1", .width = 82, .height = 64, .mark = 41},
        {.name = "SAND_ANIM00", .width = 4, .height = 21, .mark = 42},
        {.name = "SAND_ANIM01", .width = 4, .height = 21, .mark = 43},
        {.name = "SAND_ANIM02", .width = 4, .height = 21, .mark = 44},
        {.name = "SAND_ANIM03", .width = 4, .height = 21, .mark = 45},
        {.name = "SAND_ANIM04", .width = 4, .height = 21, .mark = 46},
    }};
    static constexpr std::array<N64Picture, 2> kWarriorPortraits{{
        {.name = "WAR_P1", .width = 35, .height = 33, .mark = 30},
        {.name = "WAR_P2", .width = 35, .height = 33, .mark = 31},
    }};
    static constexpr std::array<N64Picture, 2> kWizardPortraits{{
        {.name = "WIZ_P1", .width = 35, .height = 35, .mark = 32},
        {.name = "WIZ_P2", .width = 35, .height = 33, .mark = 33},
    }};
    static constexpr std::string_view kWarriorBank = "hud/warf/objects.wad";
    static constexpr std::string_view kWizardBank = "hud/wizf/objects.wad";
    /** In code order, as a font's directory runs. */
    static constexpr std::string_view kCharacters = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr N64Font kSmallFont{.file = game::LegendsN64HudPainter::kSmallFontFile,
                                        .codes = kCharacters,
                                        .columns = 4,
                                        .rows = 6,
                                        .atlasRows = 6,
                                        .lineHeight = 9,
                                        .space = 3};
    static constexpr N64Font kNameFont{.file = game::LegendsN64HudPainter::kNameFontFile,
                                       .codes = kCharacters,
                                       .columns = 6,
                                       .rows = 9,
                                       .atlasRows = 9,
                                       .lineHeight = 11,
                                       .space = 5};
    static constexpr N64Font kNumberFont{.file = game::LegendsN64HudPainter::kNumberFontFile,
                                         .codes = "0123456789",
                                         .columns = 5,
                                         .rows = 8,
                                         .atlasRows = 7,
                                         .lineHeight = 9,
                                         .space = 4,
                                         .intensityAlpha = true};

    explicit LegendsN64HudFixture(std::string_view name) : m_root(scratchDirectory(name)) {
        writeBank(game::LegendsN64HudPainter::kHudBank, kHudPictures);
        writeBank(kWarriorBank, kWarriorPortraits);
        writeBank(kWizardBank, kWizardPortraits);
        writeBank(game::LegendsN64HudPainter::kTimerBank, kTimerPictures);
        for (const N64Font& font : {kSmallFont, kNameFont, kNumberFont}) {
            write(font.file, n64FontBytes(font));
        }
    }

    const std::filesystem::path& root() const { return m_root; }

    /** Writes `bytes` as the extraction's file `name`. */
    void write(std::string_view name, std::span<const u8> bytes) const {
        const std::filesystem::path file = m_root / name;
        std::filesystem::create_directories(file.parent_path());
        writeFile(file, bytes);
    }
    /** Writes a bank of `pictures` as the extraction's file `name`. */
    void writeBank(std::string_view name, std::span<const N64Picture> pictures) const {
        write(name, n64TextureBank(pictures));
    }
    /** Cuts the extraction's file `name` to its first half. */
    void truncate(std::string_view name) const {
        const std::vector<u8> bytes = readFile(m_root / name);
        write(name, std::span{bytes}.first(bytes.size() / 2));
    }

    /** The picture `texture` shows, told by its mark and size; empty for none of the
     * extraction's. */
    static std::string_view pictureOf(const Texture* texture) {
        const auto* image = dynamic_cast<const FakeTexture*>(texture);
        if (image == nullptr || image->pixels.empty()) {
            return {};
        }
        const std::array<std::span<const N64Picture>, 4> banks{kHudPictures, kWarriorPortraits,
                                                               kWizardPortraits, kTimerPictures};
        for (const std::span<const N64Picture> bank : banks) {
            for (const N64Picture& picture : bank) {
                if (picture.mark == image->pixels.front() && picture.width == image->width() &&
                    picture.height == image->height()) {
                    return picture.name;
                }
            }
        }
        return {};
    }
    /** Whether `texture` is `font`'s sheet, told by its size. */
    static bool sheetOf(const Texture* texture, const N64Font& font) {
        return texture != nullptr && texture->width() == font.sheetWidth() &&
               texture->height() == font.sheetHeight();
    }

private:
    std::filesystem::path m_root;
};

} // namespace gdl::test
