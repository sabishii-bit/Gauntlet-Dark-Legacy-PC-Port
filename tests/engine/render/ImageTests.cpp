#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"
#include "engine/render/Image.h"

namespace {

using namespace gdl;

TEST_CASE("filled images hold the colour everywhere", "[render][image]") {
    const Image image = Image::filled(3, 2, Color::rgba(1, 2, 3, 4));
    REQUIRE(image.pixels.size() == 24);
    REQUIRE(image.rowBytes() == 12);
    REQUIRE(image.pixel(0, 0) == Color::rgba(1, 2, 3, 4));
    REQUIRE(image.pixel(2, 1) == Color::rgba(1, 2, 3, 4));
}

TEST_CASE("pixels are addressed by row and column", "[render][image]") {
    Image image = Image::filled(4, 4, Color::black());
    image.setPixel(3, 2, Color::rgba(9, 8, 7));
    REQUIRE(image.pixel(3, 2) == Color::rgba(9, 8, 7));
    REQUIRE(image.pixel(2, 3) == Color::black());
    const auto row = image.row(2);
    REQUIRE(row.size() == 16);
    REQUIRE(row[12] == 9);
    REQUIRE(row[15] == 255);
}

} // namespace
