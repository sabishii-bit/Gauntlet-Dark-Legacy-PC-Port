#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"

#include <bit>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("Dreamcast Legends texture: " + std::string(message));
    }
}

void range(std::span<const u8> bytes, usize offset, usize size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset, "truncated data");
}

usize morton(u32 x, u32 y, u32 width, u32 height) {
    usize result = 0;
    u32 shift = 0;
    for (u32 bit = 1; bit < width || bit < height; bit <<= 1U) {
        if (bit < height) {
            if ((y & bit) != 0) {
                result |= usize{1} << shift;
            }
            ++shift;
        }
        if (bit < width) {
            if ((x & bit) != 0) {
                result |= usize{1} << shift;
            }
            ++shift;
        }
    }
    return result;
}

u8 five(u16 value) {
    return static_cast<u8>((value << 3U) | (value >> 2U));
}

Color color(u16 value, u8 format) {
    if (format == 0) {
        return Color::rgba(five((value >> 10U) & 31U), five((value >> 5U) & 31U), five(value & 31U),
                           (value & 0x8000U) != 0 ? 255 : 0);
    }
    if (format == 1) {
        const u16 green = (value >> 5U) & 63U;
        return Color::rgba(five(value >> 11U), static_cast<u8>((green << 2U) | (green >> 4U)),
                           five(value & 31U), 255);
    }
    return Color::rgba(static_cast<u8>(((value >> 8U) & 15U) * 17U),
                       static_cast<u8>(((value >> 4U) & 15U) * 17U),
                       static_cast<u8>((value & 15U) * 17U), static_cast<u8>((value >> 12U) * 17U));
}

Image decode(std::span<const u8> bytes, u32 width, u32 height, u8 format, u8 kind) {
    require(format <= 2, "unsupported pixel format");
    const bool vq = kind == 3 || kind == 4 || kind == 16 || kind == 17;
    const bool mip = kind == 2 || kind == 4 || kind == 17;
    require(kind == 1 || kind == 2 || kind == 3 || kind == 4 || kind == 9 || kind == 13 ||
                kind == 16 || kind == 17,
            "unsupported storage format");
    require((!vq && !mip) || width == height, "non-square mipmapped/VQ texture");
    usize codebook = 0;
    usize offset = 0;
    if (vq) {
        require(width >= 2, "VQ texture is too small");
        codebook = 256;
        if (kind == 16 || kind == 17) {
            // Retail kmLoadTexture (8c0321f4), tables 8c057708/8c057724.
            codebook = width <= 16   ? 16
                       : width == 32 ? (mip ? 64 : 32)
                       : width == 64 ? (mip ? 256 : 128)
                                     : 256;
        }
        offset = codebook * 8;
        if (mip) {
            ++offset; // Authored 1x1 VQ index.
            for (usize dimension = 2; dimension < width; dimension *= 2) {
                offset += dimension * dimension / 4;
            }
        }
        range(bytes, 0, codebook * 8);
        range(bytes, offset, usize{width} * height / 4);
    } else {
        if (mip) {
            // The file omits four hardware padding bytes: retail kmLoadTexture
            // (8c032180) copies these mip chains to VRAM + 4. The 1x1 level
            // starts at byte 2; subsequent levels are tightly packed.
            offset = 2;
            for (usize dimension = 1; dimension < width; dimension *= 2) {
                offset += dimension * dimension * 2;
            }
        }
        range(bytes, offset, usize{width} * height * 2);
    }
    Image result = Image::filled(width, height, Color::white());
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            usize pixel = 0;
            if (vq) {
                const u8 entry = bytes[offset + morton(x / 2, y / 2, width / 2, height / 2)];
                require(entry < codebook, "VQ index outside compact codebook");
                pixel = usize{entry} * 8 + (usize{x & 1U} * 2 + (y & 1U)) * 2;
            } else {
                const usize slot = kind == 9 ? usize{y} * width + x : morton(x, y, width, height);
                pixel = offset + slot * 2;
            }
            result.setPixel(x, y, color(readU16LE(bytes, pixel), format));
        }
    }
    return result;
}

} // namespace

Image decodeDreamcastLightmap(std::span<const u8> models, std::span<const u8> pixels, u32 record) {
    // LoadLightmaps 8c0f1860 -> 8c0f6340 passes texture offset, native
    // pixel/storage format, width and height to kmCreate/LoadTextureSurface.
    range(models, record, 12);
    const usize offset = readU32LE(models, record);
    const u32 width = readU16LE(models, record + 8);
    const u32 height = readU16LE(models, record + 10);
    require(width <= 1024 && height <= 1024 && std::has_single_bit(width) &&
                std::has_single_bit(height),
            "invalid lightmap dimensions");
    range(pixels, offset, 0);
    return decode(pixels.subspan(offset), width, height, models[record + 4], models[record + 5]);
}

std::vector<LegendsMapTexture> decodeLegendsDreamcastTextures(std::span<const u8> models,
                                                              std::span<const u8> pixels) {
    range(models, 0, 104);
    require(readU32LE(models, 64) == 0xf00b0001U, "expected v1 objects.rom");
    const usize modelsCount = readU32LE(models, 96);
    const usize count = readU32LE(models, 100);
    require(modelsCount <= 16384 && count <= 4096, "table budget exceeded");
    const usize records = 104 + modelsCount * 140;
    range(models, records, count * 80);
    std::vector<LegendsMapTexture> result;
    result.reserve(count);
    usize remainingBytes = usize{128} * 1024 * 1024;
    for (usize i = 0; i < count; ++i) {
        const usize at = records + i * 80;
        require(readU16LE(models, at) == 0xff, "invalid Dreamcast bitmap signature");
        const u8 flags = models[at + 2];
        const u32 width = readU16LE(models, at + 4);
        const u32 height = readU16LE(models, at + 6);
        require(width <= 1024 && height <= 1024 && std::has_single_bit(width) &&
                    std::has_single_bit(height),
                "invalid bitmap dimensions");
        LegendsMapTexture texture;
        texture.width = width;
        texture.height = height;
        texture.clampU = (flags & 1U) != 0;
        texture.clampV = (flags & 2U) != 0;
        texture.external = (flags & 8U) != 0;
        if ((flags & 8U) == 0) {
            const usize offset = readU32LE(models, at + 8);
            const usize size = readU32LE(models, at + 20);
            range(pixels, offset, size);
            const usize decodedSize = usize{width} * height * 4;
            require(decodedSize <= remainingBytes, "decoded texture budget exceeded");
            remainingBytes -= decodedSize;
            texture.image = decode(pixels.subspan(offset, size), width, height, models[at + 16],
                                   models[at + 17]);
            for (usize p = 3; p < texture.image.pixels.size(); p += 4) {
                texture.hasTransparency |= texture.image.pixels[p] != 255;
            }
        }
        result.push_back(std::move(texture));
    }
    // MBOX_LoadModel's name table is separate from bitmap storage. Its entries
    // use 30-byte names and a signed 16-bit native bitmap index at +30.
    const usize definitionCount = readU32LE(models, 68);
    const usize definitions = readU32LE(models, 92);
    require(definitionCount <= 16384, "bitmap definition budget exceeded");
    range(models, definitions, definitionCount * 36);
    for (usize i = 0; i < definitionCount; ++i) {
        const usize at = definitions + i * 36;
        const u16 slot = readU16LE(models, at + 30);
        require(slot < count, "bitmap definition index out of bounds");
        usize length = 0;
        while (length < 30 && models[at + length] != 0) {
            ++length;
        }
        if (result[slot].name.empty()) {
            result[slot].name.assign(std::bit_cast<const char*>(models.data() + at), length);
        }
    }
    return result;
}

} // namespace gdl::formats
