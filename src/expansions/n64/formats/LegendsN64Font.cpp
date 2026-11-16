#include "expansions/n64/formats/LegendsN64Font.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/math/Math.h"

namespace gdl::formats {

namespace {

/** How a font's atlas stores its texels. */
enum class AtlasFormat : u8 { Intensity4, IntensityAlpha4, IntensityAlpha8 };

/** The atlas format a header's format and texel size bytes name, when a font may use it. */
std::optional<AtlasFormat> atlasFormat(u8 format, u8 size) {
    constexpr u8 kIntensity = 4;
    constexpr u8 kIntensityAlpha = 3;
    constexpr u8 kFourBits = 0;
    constexpr u8 kEightBits = 1;
    if (format == kIntensity && size == kFourBits) {
        return AtlasFormat::Intensity4;
    }
    if (format == kIntensityAlpha && size == kFourBits) {
        return AtlasFormat::IntensityAlpha4;
    }
    if (format == kIntensityAlpha && size == kEightBits) {
        return AtlasFormat::IntensityAlpha8;
    }
    return std::nullopt;
}

/** The bits texel `i` of a packed atlas holds: eight, or four with the high nibble first. */
u32 texelBits(std::span<const u8> packed, AtlasFormat format, usize i) {
    if (format == AtlasFormat::IntensityAlpha8) {
        return packed[i];
    }
    return (i & 1U) != 0 ? packed[i / 2] & 15U : packed[i / 2] >> 4U;
}

/** A texel's colour as LegendsN64Texture decodes its format, but for I4: a font's I4 texel is
 * coverage alone, drawn in the ink, while an IA texel's intensity shades the ink as well. */
Color atlasTexel(AtlasFormat format, u32 bits) {
    switch (format) {
    case AtlasFormat::Intensity4: return Color::rgba(255, 255, 255, static_cast<u8>(bits * 17U));
    case AtlasFormat::IntensityAlpha4: {
        // Three bits of intensity, widened by repeating them, over one bit of alpha.
        const u32 three = bits >> 1U;
        const auto intensity = static_cast<u8>((three << 5U) | (three << 2U) | (three >> 1U));
        return Color::rgba(intensity, intensity, intensity, (bits & 1U) != 0 ? 255 : 0);
    }
    case AtlasFormat::IntensityAlpha8: {
        const auto intensity = static_cast<u8>((bits >> 4U) * 17U);
        return Color::rgba(intensity, intensity, intensity, static_cast<u8>((bits & 15U) * 17U));
    }
    }
    return Color::transparent();
}

} // namespace

LegendsN64Font LegendsN64Font::parse(std::span<const u8> bytes) {
    constexpr usize kHeader = 0x24;
    constexpr usize kGlyph = 5;
    const auto format = bytes.size() < kHeader ? std::nullopt : atlasFormat(bytes[1], bytes[2]);
    if (!format) {
        throw FormatError("N64 font: expected native I4, IA4 or IA8 header");
    }
    const u32 width = readU16BE(bytes, 8);
    const u32 height = readU16BE(bytes, 10);
    const s32 lineHeight = readU16BE(bytes, 12);
    const s32 space = readU16BE(bytes, 14);
    const u32 flags = readU32BE(bytes, 16);
    const usize count = readU32BE(bytes, 20);
    if (width == 0 || height == 0 || width > 1024 || height > 1024 || lineHeight == 0 ||
        space == 0 || count == 0 || count > 256 || count > (bytes.size() - kHeader) / kGlyph) {
        throw FormatError("N64 font: invalid dimensions or glyph count");
    }
    // e0016f30 binds glyphs at +0x24 and the atlas at align16(0x24+count*5).
    const usize pixels = (kHeader + count * kGlyph + 15) & ~usize{15};
    const usize stored = usize{width} * height;
    const usize atlasBytes = *format == AtlasFormat::IntensityAlpha8 ? stored : (stored + 1) / 2;
    if (pixels > bytes.size() || atlasBytes > bytes.size() - pixels) {
        throw FormatError("N64 font: truncated atlas");
    }
    // A glyph may reach below the atlas into its line: numfont's digits claim an eighth row of
    // a seven-row atlas with a line of nine. What the N64 samples there is not known, so the
    // atlas gains clear rows down to the lowest glyph.
    const s32 deepest = std::max(static_cast<s32>(height), lineHeight);
    s32 bottom = 0;
    std::vector<BitmapGlyph> glyphs;
    u8 previous = 0;
    for (usize i = 0; i < count; ++i) {
        const usize at = kHeader + i * kGlyph;
        const u8 code = bytes[at];
        const s32 x = bytes[at + 1];
        const s32 y = bytes[at + 2];
        const s32 columns = bytes[at + 3];
        const s32 rows = bytes[at + 4];
        if (code <= previous || columns == 0 || rows == 0 ||
            x + columns > static_cast<s32>(width) || y + rows > deepest) {
            throw FormatError("N64 font: invalid glyph directory");
        }
        previous = code;
        bottom = std::max(bottom, y + rows);
        glyphs.push_back({code, columns, x, y, 0, 0, columns, rows});
        // e0016d48 folds ASCII lowercase for fonts carrying flag1.
        if ((flags & 1U) != 0 && code >= 'A' && code <= 'Z') {
            glyphs.push_back({code + ('a' - 'A'), columns, x, y, 0, 0, columns, rows});
        }
    }
    LegendsN64Font result;
    result.font = BitmapFont::fromGlyphs(lineHeight, space, std::move(glyphs));
    result.atlas.width = width;
    result.atlas.height = std::max(height, static_cast<u32>(bottom));
    const usize texels = usize{width} * result.atlas.height;
    result.atlas.pixels.resize(texels * 4);
    const auto packed = bytes.subspan(pixels, atlasBytes);
    for (usize i = 0; i < texels; ++i) {
        // The added rows hold zero texels, which are clear in every format.
        const u32 bits = i < stored ? texelBits(packed, *format, i) : 0;
        result.atlas.setPixel(static_cast<u32>(i % width), static_cast<u32>(i / width),
                              atlasTexel(*format, bits));
    }
    return result;
}

} // namespace gdl::formats
