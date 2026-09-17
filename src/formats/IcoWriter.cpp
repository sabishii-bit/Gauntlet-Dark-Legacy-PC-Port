#include "formats/IcoWriter.h"

#include <algorithm>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

constexpr usize kDirectoryHeaderSize = 6;
constexpr usize kDirectoryEntrySize = 16;
constexpr usize kBitmapHeaderSize = 40;
constexpr u32 kMaxIconSize = 256;
constexpr u16 kBitsPerPixel = 32;
constexpr u32 kBitsPerByte = 8;

void putU16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFU));
    out.push_back(static_cast<u8>(value >> 8U));
}

void putU32(std::vector<u8>& out, u32 value) {
    for (u32 shift = 0; shift < 32; shift += kBitsPerByte) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFU));
    }
}

/** Mask rows are 1 bit per pixel, padded to four bytes. */
usize maskRowBytes(u32 width) {
    return ((usize{width} + 31) / 32) * 4;
}

usize bitmapBytes(const Image& image) {
    return kBitmapHeaderSize + image.rowBytes() * image.height +
           maskRowBytes(image.width) * image.height;
}

/** One entry's bitmap: the header, BGRA rows bottom-up, then the mask rows bottom-up. */
void appendBitmap(std::vector<u8>& out, const Image& image) {
    putU32(out, static_cast<u32>(kBitmapHeaderSize));
    putU32(out, image.width);
    putU32(out, image.height * 2); // the colour rows plus the mask rows
    putU16(out, 1);
    putU16(out, kBitsPerPixel);
    putU32(out, 0); // uncompressed
    putU32(out, static_cast<u32>(image.rowBytes() * image.height));
    putU32(out, 0);
    putU32(out, 0);
    putU32(out, 0);
    putU32(out, 0);
    for (u32 y = image.height; y > 0; --y) {
        const std::span<const u8> row = image.row(y - 1);
        for (u32 x = 0; x < image.width; ++x) {
            const usize p = usize{x} * 4;
            out.push_back(row[p + 2]);
            out.push_back(row[p + 1]);
            out.push_back(row[p]);
            out.push_back(row[p + 3]);
        }
    }
    std::vector<u8> mask(maskRowBytes(image.width));
    for (u32 y = image.height; y > 0; --y) {
        std::ranges::fill(mask, 0);
        for (u32 x = 0; x < image.width; ++x) {
            if (image.pixel(x, y - 1).a == 0) {
                mask[x / kBitsPerByte] |= static_cast<u8>(0x80U >> (x % kBitsPerByte));
            }
        }
        out.insert(out.end(), mask.begin(), mask.end());
    }
}

} // namespace

Image enlargeImage(const Image& image, u32 factor) {
    Image out =
        Image::filled(image.width * factor, image.height * factor, Color::rgba(0, 0, 0, 0));
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            out.setPixel(x, y, image.pixel(x / factor, y / factor));
        }
    }
    return out;
}

std::vector<u8> encodeIco(std::span<const Image> images) {
    std::vector<u8> out;
    putU16(out, 0);
    putU16(out, 1); // an icon rather than a cursor
    putU16(out, static_cast<u16>(images.size()));
    usize offset = kDirectoryHeaderSize + kDirectoryEntrySize * images.size();
    for (const Image& image : images) {
        if (image.width == 0 || image.height == 0 || image.width > kMaxIconSize ||
            image.height > kMaxIconSize) {
            throw FormatError("ico: images must be 1 to 256 pixels on each side");
        }
        out.push_back(static_cast<u8>(image.width % kMaxIconSize)); // 256 is written as 0
        out.push_back(static_cast<u8>(image.height % kMaxIconSize));
        out.push_back(0); // no palette
        out.push_back(0);
        putU16(out, 1);
        putU16(out, kBitsPerPixel);
        putU32(out, static_cast<u32>(bitmapBytes(image)));
        putU32(out, static_cast<u32>(offset));
        offset += bitmapBytes(image);
    }
    for (const Image& image : images) {
        appendBitmap(out, image);
    }
    return out;
}

} // namespace gdl::formats
