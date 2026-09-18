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

TEST_CASE("colour bleeds from opaque texels into the transparent ones around them",
          "[render][image]") {
    // A red dot at the centre of a transparent, black-behind-alpha square.
    Image image = Image::filled(5, 5, Color::rgba(0, 0, 0, 0));
    image.setPixel(2, 2, Color::rgba(200, 0, 0, 255));
    image.bleedIntoTransparent();
    REQUIRE(image.pixel(2, 2) == Color::rgba(200, 0, 0, 255)); // opaque texels keep everything
    REQUIRE(image.pixel(1, 1) == Color::rgba(200, 0, 0, 0));   // neighbours take its colour
    REQUIRE(image.pixel(0, 0) == Color::rgba(200, 0, 0, 0));   // and it spreads to the edge
    REQUIRE(image.pixel(4, 0).a == 0);                          // alpha is never touched
    // Two colours meet halfway.
    Image pair = Image::filled(3, 1, Color::rgba(0, 0, 0, 0));
    pair.setPixel(0, 0, Color::rgba(100, 0, 0, 255));
    pair.setPixel(2, 0, Color::rgba(0, 0, 200, 255));
    pair.bleedIntoTransparent();
    REQUIRE(pair.pixel(1, 0) == Color::rgba(50, 0, 100, 0));
    // Nothing opaque: nothing to bleed. Empty images are fine too.
    Image clear = Image::filled(2, 2, Color::rgba(7, 7, 7, 0));
    clear.bleedIntoTransparent();
    REQUIRE(clear.pixel(1, 1) == Color::rgba(7, 7, 7, 0));
    Image none;
    none.bleedIntoTransparent();
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
