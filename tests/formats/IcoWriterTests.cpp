#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

#include "formats/IcoWriter.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

u32 readU32(const std::vector<u8>& bytes, usize offset) {
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8U) | (u32{bytes[offset + 2]} << 16U) |
           (u32{bytes[offset + 3]} << 24U);
}

TEST_CASE("pixel art enlarges by repeating pixels", "[formats][ico]") {
    Image image = Image::filled(2, 1, Color::rgba(1, 2, 3, 4));
    image.setPixel(1, 0, Color::rgba(9, 9, 9, 9));
    const Image big = enlargeImage(image, 2);
    REQUIRE(big.width == 4);
    REQUIRE(big.height == 2);
    REQUIRE(big.pixel(1, 1) == Color::rgba(1, 2, 3, 4));
    REQUIRE(big.pixel(2, 0) == Color::rgba(9, 9, 9, 9));
}

TEST_CASE("icons hold BGRA bitmaps with a transparency mask", "[formats][ico]") {
    Image image = Image::filled(2, 2, Color::rgba(10, 20, 30, 255));
    image.setPixel(1, 0, Color::rgba(0, 0, 0, 0));
    const std::array<Image, 2> images{image, enlargeImage(image, 128)};
    const std::vector<u8> ico = encodeIco(images);

    // The directory: two icon entries, the second 256 pixels wide (written as 0).
    REQUIRE(ico[2] == 1);
    REQUIRE(ico[4] == 2);
    REQUIRE(ico[6] == 2);
    REQUIRE(ico[7] == 2);
    REQUIRE(ico[22] == 0);
    REQUIRE(ico[23] == 0);
    REQUIRE(readU32(ico, 14) == 40 + 16 + 8); // header, four BGRA pixels, two mask rows
    REQUIRE(readU32(ico, 18) == 38);

    // The first bitmap: a header with the height doubled, then rows bottom-up.
    REQUIRE(readU32(ico, 38) == 40);
    REQUIRE(readU32(ico, 46) == 4);
    REQUIRE(ico[78] == 30);
    REQUIRE(ico[79] == 20);
    REQUIRE(ico[80] == 10);
    REQUIRE(ico[81] == 255);
    REQUIRE(ico[93] == 0); // the top row's second pixel is transparent
    REQUIRE(ico[94] == 0x00);
    REQUIRE(ico[98] == 0x40); // and so is masked out
    REQUIRE(ico.size() == 38 + 64 + (40 + 256 * 256 * 4 + 32 * 256));
}

TEST_CASE("oversized and empty icons are refused", "[formats][ico]") {
    const std::array<Image, 1> wide{Image::filled(257, 1, Color::white())};
    REQUIRE_THROWS_AS(encodeIco(wide), FormatError);
    const std::array<Image, 1> empty{Image{}};
    REQUIRE_THROWS_AS(encodeIco(empty), FormatError);
}

} // namespace
