#include "formats/IcoWriter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kDirectoryHeaderSize = 6;
constexpr std::size_t kDirectoryEntrySize = 16;
constexpr std::size_t kBitmapHeaderSize = 40;
constexpr std::uint32_t kMaxIconSize = 256;
constexpr std::uint16_t kBitsPerPixel = 32;
constexpr std::uint32_t kBitsPerByte = 8;

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += kBitsPerByte) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/** Mask rows are 1 bit per pixel, padded to four bytes. */
std::size_t maskRowBytes(std::uint32_t width) {
    return ((std::size_t{width} + 31) / 32) * 4;
}

std::size_t bitmapBytes(const Image& image) {
    return kBitmapHeaderSize + image.rowBytes() * image.height +
           maskRowBytes(image.width) * image.height;
}

/** One entry's bitmap: the header, BGRA rows bottom-up, then the mask rows bottom-up. */
void appendBitmap(std::vector<std::uint8_t>& out, const Image& image) {
    putU32(out, static_cast<std::uint32_t>(kBitmapHeaderSize));
    putU32(out, image.width);
    putU32(out, image.height * 2); // the colour rows plus the mask rows
    putU16(out, 1);
    putU16(out, kBitsPerPixel);
    putU32(out, 0); // uncompressed
    putU32(out, static_cast<std::uint32_t>(image.rowBytes() * image.height));
    putU32(out, 0);
    putU32(out, 0);
    putU32(out, 0);
    putU32(out, 0);
    for (std::uint32_t y = image.height; y > 0; --y) {
        const std::span<const std::uint8_t> row = image.row(y - 1);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t p = std::size_t{x} * 4;
            out.push_back(row[p + 2]);
            out.push_back(row[p + 1]);
            out.push_back(row[p]);
            out.push_back(row[p + 3]);
        }
    }
    std::vector<std::uint8_t> mask(maskRowBytes(image.width));
    for (std::uint32_t y = image.height; y > 0; --y) {
        std::ranges::fill(mask, 0);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            if (image.pixel(x, y - 1).a == 0) {
                mask[x / kBitsPerByte] |= static_cast<std::uint8_t>(0x80U >> (x % kBitsPerByte));
            }
        }
        out.insert(out.end(), mask.begin(), mask.end());
    }
}

} // namespace

Image enlargeImage(const Image& image, std::uint32_t factor) {
    Image out = Image::filled(image.width * factor, image.height * factor, Color::rgba(0, 0, 0, 0));
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            out.setPixel(x, y, image.pixel(x / factor, y / factor));
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeIco(std::span<const Image> images) {
    std::vector<std::uint8_t> out;
    putU16(out, 0);
    putU16(out, 1); // an icon rather than a cursor
    putU16(out, static_cast<std::uint16_t>(images.size()));
    std::size_t offset = kDirectoryHeaderSize + kDirectoryEntrySize * images.size();
    for (const Image& image : images) {
        if (image.width == 0 || image.height == 0 || image.width > kMaxIconSize ||
            image.height > kMaxIconSize) {
            throw FormatError("ico: images must be 1 to 256 pixels on each side");
        }
        out.push_back(static_cast<std::uint8_t>(image.width % kMaxIconSize)); // 256 is written as 0
        out.push_back(static_cast<std::uint8_t>(image.height % kMaxIconSize));
        out.push_back(0); // no palette
        out.push_back(0);
        putU16(out, 1);
        putU16(out, kBitsPerPixel);
        putU32(out, static_cast<std::uint32_t>(bitmapBytes(image)));
        putU32(out, static_cast<std::uint32_t>(offset));
        offset += bitmapBytes(image);
    }
    for (const Image& image : images) {
        appendBitmap(out, image);
    }
    return out;
}

} // namespace gdl::formats
