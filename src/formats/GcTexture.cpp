#include "formats/GcTexture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

using namespace bitmap_format;

constexpr std::size_t kPaletteEntries4 = 16;
constexpr std::size_t kPaletteEntries8 = 256;

struct Tile {
    std::uint32_t width;
    std::uint32_t height;
};

/** GameCube tile dimensions by bits per pixel. */
Tile tileFor(std::uint32_t bits) {
    switch (bits) {
    case 4: return Tile{8, 8};
    case 8: return Tile{8, 4};
    default: return Tile{4, 4};
    }
}

enum class Coding : std::uint8_t { Direct16, Indexed4, Indexed8, Intensity4, Intensity8 };

Coding codingFor(std::uint8_t format) {
    switch (format) {
    case kRgb5a3:
    case kRgb5a3NoAlpha: return Coding::Direct16;
    case kIndexed4:
    case kIndexed4NoAlpha:
    case kIndexed4Gc:
    case kIndexed4Wide:
    case kIndexed4WideNoAlpha: return Coding::Indexed4;
    case kIndexed8:
    case kIndexed8NoAlpha:
    case kIndexed8Gc:
    case kIntensityAlpha8:
    case kIndexed8Wide:
    case kIndexed8WideNoAlpha: return Coding::Indexed8;
    case kAlpha4:
    case kIntensity4: return Coding::Intensity4;
    case kAlpha8:
    case kIntensity8: return Coding::Intensity8;
    default: throw FormatError(std::format("unsupported texture format {}", format));
    }
}

std::uint16_t readBigU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((std::uint32_t{bytes[offset]} << 8U) | bytes[offset + 1]);
}

/** Index of texel (x, y) within tiled data measured in texels. */
std::size_t tiledIndex(std::uint32_t x, std::uint32_t y, std::uint32_t width, Tile tile) {
    const std::uint32_t tilesPerRow = (width + tile.width - 1) / tile.width;
    const std::uint32_t tileIndex = (y / tile.height) * tilesPerRow + (x / tile.width);
    const std::uint32_t inTile = (y % tile.height) * tile.width + (x % tile.width);
    return std::size_t{tileIndex} * tile.width * tile.height + inTile;
}

std::uint32_t tiledTexelCount(std::uint32_t width, std::uint32_t height, Tile tile) {
    const std::uint32_t tilesX = (width + tile.width - 1) / tile.width;
    const std::uint32_t tilesY = (height + tile.height - 1) / tile.height;
    return tilesX * tilesY * tile.width * tile.height;
}

/** Expands a 4-bit intensity into the alpha ramp the game's built-in palette produces. */
constexpr std::uint8_t expand4(std::uint32_t value) {
    return static_cast<std::uint8_t>(value * 17);
}

} // namespace

Color rgb5a3ToColor(std::uint16_t texel) {
    if ((texel & 0x8000U) != 0) {
        const std::uint32_t r = (texel >> 10U) & 0x1FU;
        const std::uint32_t g = (texel >> 5U) & 0x1FU;
        const std::uint32_t b = texel & 0x1FU;
        return Color::rgba(static_cast<std::uint8_t>((r << 3U) | (r >> 2U)),
                           static_cast<std::uint8_t>((g << 3U) | (g >> 2U)),
                           static_cast<std::uint8_t>((b << 3U) | (b >> 2U)));
    }
    const std::uint32_t a = (texel >> 12U) & 0x7U;
    const std::uint32_t r = (texel >> 8U) & 0xFU;
    const std::uint32_t g = (texel >> 4U) & 0xFU;
    const std::uint32_t b = texel & 0xFU;
    return Color::rgba(expand4(r), expand4(g), expand4(b),
                       static_cast<std::uint8_t>((a << 5U) | (a << 2U) | (a >> 1U)));
}

std::uint32_t bitsPerPixel(std::uint8_t format) {
    switch (codingFor(format)) {
    case Coding::Direct16: return 16;
    case Coding::Indexed4:
    case Coding::Intensity4: return 4;
    case Coding::Indexed8:
    case Coding::Intensity8: return 8;
    }
    return 0;
}

std::size_t pixelDataOffset(std::uint8_t format) {
    switch (format) {
    case kIndexed4:
    case kIndexed4NoAlpha:
    case kIndexed4Gc: return kPaletteEntries4 * 2;
    case kIndexed4Wide:
    case kIndexed4WideNoAlpha: return kPaletteEntries4 * 4;
    case kIndexed8:
    case kIndexed8NoAlpha:
    case kIndexed8Gc:
    case kIntensityAlpha8: return kPaletteEntries8 * 2;
    case kIndexed8Wide:
    case kIndexed8WideNoAlpha: return kPaletteEntries8 * 4;
    default: return 0;
    }
}

std::size_t bitmapDataSize(const ArchiveBitmap& bitmap) {
    const std::uint32_t bits = bitsPerPixel(bitmap.format);
    const Tile tile = tileFor(bits);
    std::size_t size = pixelDataOffset(bitmap.format);
    std::uint32_t width = bitmap.width;
    std::uint32_t height = bitmap.height;
    for (std::uint32_t level = 0; level <= bitmap.mipmapCount; ++level) {
        size += std::size_t{tiledTexelCount(width, height, tile)} * bits / 8;
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    return size;
}

Image decodeGcTexture(const ArchiveBitmap& bitmap, std::span<const std::uint8_t> texturesFile) {
    if (bitmap.width == 0 || bitmap.height == 0) {
        throw FormatError("texture has no size");
    }
    const Coding coding = codingFor(bitmap.format);
    const std::uint32_t bits = bitsPerPixel(bitmap.format);
    const Tile tile = tileFor(bits);
    const std::size_t pixelsAt = bitmap.dataOffset + pixelDataOffset(bitmap.format);
    const std::size_t pixelBytes =
        std::size_t{tiledTexelCount(bitmap.width, bitmap.height, tile)} * bits / 8;
    if (pixelsAt > texturesFile.size() || pixelBytes > texturesFile.size() - pixelsAt) {
        throw FormatError("texture pixel data lies outside the textures file");
    }
    const std::span<const std::uint8_t> pixels = texturesFile.subspan(pixelsAt, pixelBytes);

    std::array<Color, kPaletteEntries8> palette{};
    if (coding == Coding::Indexed4 || coding == Coding::Indexed8) {
        const std::uint32_t entries =
            coding == Coding::Indexed4 ? kPaletteEntries4 : kPaletteEntries8;
        const std::span<const std::uint8_t> paletteBytes =
            texturesFile.subspan(bitmap.dataOffset, std::size_t{entries} * 2);
        for (std::uint32_t i = 0; i < entries; ++i) {
            palette[i] = rgb5a3ToColor(readBigU16(paletteBytes, std::size_t{i} * 2));
        }
    }

    Image image = Image::filled(bitmap.width, bitmap.height, Color::transparent());
    for (std::uint32_t y = 0; y < bitmap.height; ++y) {
        for (std::uint32_t x = 0; x < bitmap.width; ++x) {
            const std::size_t texel = tiledIndex(x, y, bitmap.width, tile);
            Color color;
            switch (coding) {
            case Coding::Direct16: color = rgb5a3ToColor(readBigU16(pixels, texel * 2)); break;
            case Coding::Indexed8: color = palette[pixels[texel]]; break;
            case Coding::Indexed4: {
                const std::uint8_t pair = pixels[texel / 2];
                color = palette[(texel % 2 == 0) ? (pair >> 4U) : (pair & 0x0FU)];
                break;
            }
            case Coding::Intensity8: color = Color::rgba(255, 255, 255, pixels[texel]); break;
            case Coding::Intensity4: {
                const std::uint8_t pair = pixels[texel / 2];
                color = Color::rgba(255, 255, 255,
                                    expand4((texel % 2 == 0) ? (pair >> 4U) : (pair & 0x0FU)));
                break;
            }
            }
            image.setPixel(x, y, color);
        }
    }
    return image;
}

} // namespace gdl::formats
