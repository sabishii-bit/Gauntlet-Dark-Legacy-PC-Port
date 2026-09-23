#include "formats/TplFile.h"

#include <format>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

#include "formats/GcTexture.h"

namespace gdl::formats {

namespace {

constexpr u32 kMagic = 0x0020AF30;
constexpr usize kHeaderSize = 12;
constexpr usize kTableEntrySize = 8;
constexpr u32 kTile = 4;
constexpr usize kTexelsPerTile = usize{kTile} * kTile;
constexpr usize kRgba8TileBytes = 64; ///< an AR plane then a GB plane, 32 bytes each
constexpr usize kRgba8PlaneBytes = 32;

u16 readBigU16(std::span<const u8> bytes, usize offset) {
    if (offset + 2 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return static_cast<u16>((u32{bytes[offset]} << 8U) | bytes[offset + 1]);
}

u32 readBigU32(std::span<const u8> bytes, usize offset) {
    if (offset + 4 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return (u32{bytes[offset]} << 24U) | (u32{bytes[offset + 1]} << 16U) |
           (u32{bytes[offset + 2]} << 8U) | bytes[offset + 3];
}

Color rgb565ToColor(u16 texel) {
    const u32 r = (texel >> 11U) & 0x1FU;
    const u32 g = (texel >> 5U) & 0x3FU;
    const u32 b = texel & 0x1FU;
    return Color::rgba(static_cast<u8>((r << 3U) | (r >> 2U)),
                       static_cast<u8>((g << 2U) | (g >> 4U)),
                       static_cast<u8>((b << 3U) | (b >> 2U)));
}

Color rgba8Texel(std::span<const u8> bytes, usize tile, usize inTile) {
    const usize ar = tile + inTile * 2;
    const usize gb = tile + kRgba8PlaneBytes + inTile * 2;
    if (gb + 2 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return Color::rgba(bytes[ar + 1], bytes[gb], bytes[gb + 1], bytes[ar]);
}

Image decodeTiled(std::span<const u8> bytes, usize dataOffset, u32 width, u32 height, u32 format) {
    Image image = Image::filled(width, height, Color::rgba(0, 0, 0, 0));
    const usize tilesPerRow = (usize{width} + kTile - 1) / kTile;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize tileIndex = usize{y / kTile} * tilesPerRow + (x / kTile);
            const usize inTile = usize{y % kTile} * kTile + (x % kTile);
            Color color;
            if (format == kTplRgba8) {
                color = rgba8Texel(bytes, dataOffset + tileIndex * kRgba8TileBytes, inTile);
            } else {
                const usize texel = dataOffset + (tileIndex * kTexelsPerTile + inTile) * 2;
                const u16 value = readBigU16(bytes, texel);
                color = format == kTplRgb5a3 ? rgb5a3ToColor(value) : rgb565ToColor(value);
            }
            image.setPixel(x, y, color);
        }
    }
    return image;
}

} // namespace

std::vector<TplImage> parseTplFile(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderSize || readBigU32(bytes, 0) != kMagic) {
        throw FormatError("tpl: not a texture palette library");
    }
    const u32 count = readBigU32(bytes, 4);
    const u32 table = readBigU32(bytes, 8);
    std::vector<TplImage> images;
    for (u32 i = 0; i < count; ++i) {
        const usize entry = usize{table} + usize{i} * kTableEntrySize;
        const u32 header = readBigU32(bytes, entry);
        const u32 height = readBigU16(bytes, header);
        const u32 width = readBigU16(bytes, header + 2);
        const u32 format = readBigU32(bytes, header + 4);
        const u32 dataOffset = readBigU32(bytes, header + 8);
        if (format != kTplRgb565 && format != kTplRgb5a3 && format != kTplRgba8) {
            throw FormatError(std::format("tpl: image {} uses unsupported format {}", i, format));
        }
        if (width == 0 || height == 0) {
            throw FormatError(std::format("tpl: image {} is empty", i));
        }
        images.push_back(TplImage{format, decodeTiled(bytes, dataOffset, width, height, format)});
    }
    return images;
}

} // namespace gdl::formats
