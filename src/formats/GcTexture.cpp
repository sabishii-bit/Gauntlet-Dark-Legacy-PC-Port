#include "formats/GcTexture.h"

#include <array>
#include <format>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

using namespace bitmap_format;

constexpr usize kPaletteEntries4 = 16;
constexpr usize kPaletteEntries8 = 256;

struct Tile {
    u32 width;
    u32 height;
};

/** GameCube tile dimensions by bits per pixel. */
Tile tileFor(u32 bits) {
    switch (bits) {
    case 4: return Tile{8, 8};
    case 8: return Tile{8, 4};
    default: return Tile{4, 4};
    }
}

enum class Coding : u8 { Direct16, Indexed4, Indexed8, Intensity4, Intensity8 };

Coding codingFor(u8 format) {
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

u16 readBigU16(std::span<const u8> bytes, usize offset) {
    return static_cast<u16>((u32{bytes[offset]} << 8U) | bytes[offset + 1]);
}

/** Index of texel (x, y) within tiled data measured in texels. */
usize tiledIndex(u32 x, u32 y, u32 width, Tile tile) {
    const u32 tilesPerRow = (width + tile.width - 1) / tile.width;
    const u32 tileIndex = (y / tile.height) * tilesPerRow + (x / tile.width);
    const u32 inTile = (y % tile.height) * tile.width + (x % tile.width);
    return usize{tileIndex} * tile.width * tile.height + inTile;
}

u32 tiledTexelCount(u32 width, u32 height, Tile tile) {
    const u32 tilesX = (width + tile.width - 1) / tile.width;
    const u32 tilesY = (height + tile.height - 1) / tile.height;
    return tilesX * tilesY * tile.width * tile.height;
}

/** Expands a 4-bit intensity into the alpha ramp the game's built-in palette produces. */
constexpr u8 expand4(u32 value) {
    return static_cast<u8>(value * 17);
}

} // namespace

Color rgb5a3ToColor(u16 texel) {
    if ((texel & 0x8000U) != 0) {
        const u32 r = (texel >> 10U) & 0x1FU;
        const u32 g = (texel >> 5U) & 0x1FU;
        const u32 b = texel & 0x1FU;
        return Color::rgba(static_cast<u8>((r << 3U) | (r >> 2U)),
                           static_cast<u8>((g << 3U) | (g >> 2U)),
                           static_cast<u8>((b << 3U) | (b >> 2U)));
    }
    const u32 a = (texel >> 12U) & 0x7U;
    const u32 r = (texel >> 8U) & 0xFU;
    const u32 g = (texel >> 4U) & 0xFU;
    const u32 b = texel & 0xFU;
    return Color::rgba(expand4(r), expand4(g), expand4(b),
                       static_cast<u8>((a << 5U) | (a << 2U) | (a >> 1U)));
}

u32 bitsPerPixel(u8 format) {
    switch (codingFor(format)) {
    case Coding::Direct16: return 16;
    case Coding::Indexed4:
    case Coding::Intensity4: return 4;
    case Coding::Indexed8:
    case Coding::Intensity8: return 8;
    }
    return 0;
}

usize pixelDataOffset(u8 format) {
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

usize bitmapDataSize(const ArchiveBitmap& bitmap) {
    const u32 bits = bitsPerPixel(bitmap.format);
    const Tile tile = tileFor(bits);
    usize size = pixelDataOffset(bitmap.format);
    u32 width = bitmap.width;
    u32 height = bitmap.height;
    for (u32 level = 0; level <= bitmap.mipmapCount; ++level) {
        size += usize{tiledTexelCount(width, height, tile)} * bits / 8;
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    return size;
}

Image decodeGcTexture(const ArchiveBitmap& bitmap, std::span<const u8> texturesFile) {
    if (bitmap.width == 0 || bitmap.height == 0) {
        throw FormatError("texture has no size");
    }
    const Coding coding = codingFor(bitmap.format);
    const u32 bits = bitsPerPixel(bitmap.format);
    const Tile tile = tileFor(bits);
    const usize pixelsAt = bitmap.dataOffset + pixelDataOffset(bitmap.format);
    const usize pixelBytes = usize{tiledTexelCount(bitmap.width, bitmap.height, tile)} * bits / 8;
    if (pixelsAt > texturesFile.size() || pixelBytes > texturesFile.size() - pixelsAt) {
        throw FormatError("texture pixel data lies outside the textures file");
    }
    const std::span<const u8> pixels = texturesFile.subspan(pixelsAt, pixelBytes);

    std::array<Color, kPaletteEntries8> palette{};
    if (coding == Coding::Indexed4 || coding == Coding::Indexed8) {
        const u32 entries = coding == Coding::Indexed4 ? kPaletteEntries4 : kPaletteEntries8;
        const std::span<const u8> paletteBytes =
            texturesFile.subspan(bitmap.dataOffset, usize{entries} * 2);
        for (u32 i = 0; i < entries; ++i) {
            palette[i] = rgb5a3ToColor(readBigU16(paletteBytes, usize{i} * 2));
        }
    }

    Image image = Image::filled(bitmap.width, bitmap.height, Color::transparent());
    for (u32 y = 0; y < bitmap.height; ++y) {
        for (u32 x = 0; x < bitmap.width; ++x) {
            const usize texel = tiledIndex(x, y, bitmap.width, tile);
            Color color;
            switch (coding) {
            case Coding::Direct16: color = rgb5a3ToColor(readBigU16(pixels, texel * 2)); break;
            case Coding::Indexed8: color = palette[pixels[texel]]; break;
            case Coding::Indexed4: {
                const u8 pair = pixels[texel / 2];
                color = palette[(texel % 2 == 0) ? (pair >> 4U) : (pair & 0x0FU)];
                break;
            }
            case Coding::Intensity8: color = Color::rgba(255, 255, 255, pixels[texel]); break;
            case Coding::Intensity4: {
                const u8 pair = pixels[texel / 2];
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
