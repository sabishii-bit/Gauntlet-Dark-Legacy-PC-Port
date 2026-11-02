#include "formats/IcoWriter.h"

#include <algorithm>
#include <span>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteWriter.h"

namespace gdl::formats {

namespace {

constexpr usize kDirectoryHeaderSize = 6;
constexpr usize kDirectoryEntrySize = 16;
constexpr usize kBitmapHeaderSize = 40;
constexpr u32 kMaxIconSize = 256;
constexpr u16 kBitsPerPixel = 32;
constexpr u32 kBitsPerByte = 8;

/** Mask rows are 1 bit per pixel, padded to four bytes. */
usize maskRowBytes(u32 width) {
    return ((usize{width} + 31) / 32) * 4;
}

usize bitmapBytes(const Image& image) {
    return kBitmapHeaderSize + image.rowBytes() * image.height +
           maskRowBytes(image.width) * image.height;
}

/** One entry's bitmap: the header, BGRA rows bottom-up, then the mask rows bottom-up. */
void appendBitmap(ByteWriter& out, const Image& image) {
    out.writeU32(static_cast<u32>(kBitmapHeaderSize));
    out.writeU32(image.width);
    out.writeU32(image.height * 2); // the colour rows plus the mask rows
    out.writeU16(1);
    out.writeU16(kBitsPerPixel);
    out.writeU32(0); // uncompressed
    out.writeU32(static_cast<u32>(image.rowBytes() * image.height));
    out.writeU32(0);
    out.writeU32(0);
    out.writeU32(0);
    out.writeU32(0);
    for (u32 y = image.height; y > 0; --y) {
        const std::span<const u8> row = image.row(y - 1);
        for (u32 x = 0; x < image.width; ++x) {
            const usize p = usize{x} * 4;
            out.writeU8(row[p + 2]);
            out.writeU8(row[p + 1]);
            out.writeU8(row[p]);
            out.writeU8(row[p + 3]);
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
        out.writeBytes(mask);
    }
}

} // namespace

Image enlargeImage(const Image& image, u32 factor) {
    Image out = Image::filled(image.width * factor, image.height * factor, Color::rgba(0, 0, 0, 0));
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            out.setPixel(x, y, image.pixel(x / factor, y / factor));
        }
    }
    return out;
}

std::vector<u8> encodeIco(std::span<const Image> images) {
    std::vector<u8> bytes;
    ByteWriter out(bytes);
    out.writeU16(0);
    out.writeU16(1); // an icon rather than a cursor
    out.writeU16(static_cast<u16>(images.size()));
    usize offset = kDirectoryHeaderSize + kDirectoryEntrySize * images.size();
    for (const Image& image : images) {
        if (image.width == 0 || image.height == 0 || image.width > kMaxIconSize ||
            image.height > kMaxIconSize) {
            throw FormatError("ico: images must be 1 to 256 pixels on each side");
        }
        out.writeU8(static_cast<u8>(image.width % kMaxIconSize)); // 256 is written as 0
        out.writeU8(static_cast<u8>(image.height % kMaxIconSize));
        out.writeU8(0); // no palette
        out.writeU8(0);
        out.writeU16(1);
        out.writeU16(kBitsPerPixel);
        out.writeU32(static_cast<u32>(bitmapBytes(image)));
        out.writeU32(static_cast<u32>(offset));
        offset += bitmapBytes(image);
    }
    for (const Image& image : images) {
        appendBitmap(out, image);
    }
    return bytes;
}

} // namespace gdl::formats
