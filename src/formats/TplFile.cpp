#include "formats/TplFile.h"

#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Error.h"

#include "formats/GcTexture.h"

namespace gdl::formats {

namespace {

constexpr std::uint32_t kMagic = 0x0020AF30;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kTableEntrySize = 8;
constexpr std::uint32_t kTile = 4;
constexpr std::size_t kTexelsPerTile = std::size_t{kTile} * kTile;
constexpr std::size_t kRgba8TileBytes = 64; ///< an AR plane then a GB plane, 32 bytes each
constexpr std::size_t kRgba8PlaneBytes = 32;

std::uint16_t readBigU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return static_cast<std::uint16_t>((std::uint32_t{bytes[offset]} << 8U) | bytes[offset + 1]);
}

std::uint32_t readBigU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return (std::uint32_t{bytes[offset]} << 24U) | (std::uint32_t{bytes[offset + 1]} << 16U) |
           (std::uint32_t{bytes[offset + 2]} << 8U) | bytes[offset + 3];
}

Color rgb565ToColor(std::uint16_t texel) {
    const std::uint32_t r = (texel >> 11U) & 0x1FU;
    const std::uint32_t g = (texel >> 5U) & 0x3FU;
    const std::uint32_t b = texel & 0x1FU;
    return Color::rgba(static_cast<std::uint8_t>((r << 3U) | (r >> 2U)),
                       static_cast<std::uint8_t>((g << 2U) | (g >> 4U)),
                       static_cast<std::uint8_t>((b << 3U) | (b >> 2U)));
}

Color rgba8Texel(std::span<const std::uint8_t> bytes, std::size_t tile, std::size_t inTile) {
    const std::size_t ar = tile + inTile * 2;
    const std::size_t gb = tile + kRgba8PlaneBytes + inTile * 2;
    if (gb + 2 > bytes.size()) {
        throw FormatError("tpl: truncated");
    }
    return Color::rgba(bytes[ar + 1], bytes[gb], bytes[gb + 1], bytes[ar]);
}

Image decodeTiled(std::span<const std::uint8_t> bytes, std::size_t dataOffset, std::uint32_t width,
                  std::uint32_t height, std::uint32_t format) {
    Image image = Image::filled(width, height, Color::rgba(0, 0, 0, 0));
    const std::size_t tilesPerRow = (std::size_t{width} + kTile - 1) / kTile;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t tileIndex = std::size_t{y / kTile} * tilesPerRow + (x / kTile);
            const std::size_t inTile = std::size_t{y % kTile} * kTile + (x % kTile);
            Color color;
            if (format == kTplRgba8) {
                color = rgba8Texel(bytes, dataOffset + tileIndex * kRgba8TileBytes, inTile);
            } else {
                const std::size_t texel = dataOffset + (tileIndex * kTexelsPerTile + inTile) * 2;
                const std::uint16_t value = readBigU16(bytes, texel);
                color = format == kTplRgb5a3 ? rgb5a3ToColor(value) : rgb565ToColor(value);
            }
            image.setPixel(x, y, color);
        }
    }
    return image;
}

} // namespace

std::vector<TplImage> parseTplFile(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || readBigU32(bytes, 0) != kMagic) {
        throw FormatError("tpl: not a texture palette library");
    }
    const std::uint32_t count = readBigU32(bytes, 4);
    const std::uint32_t table = readBigU32(bytes, 8);
    std::vector<TplImage> images;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = std::size_t{table} + std::size_t{i} * kTableEntrySize;
        const std::uint32_t header = readBigU32(bytes, entry);
        const std::uint32_t height = readBigU16(bytes, header);
        const std::uint32_t width = readBigU16(bytes, header + 2);
        const std::uint32_t format = readBigU32(bytes, header + 4);
        const std::uint32_t dataOffset = readBigU32(bytes, header + 8);
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
