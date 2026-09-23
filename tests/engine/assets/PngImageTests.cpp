#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/PngImage.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("a PNG decodes to RGBA8 rows top to bottom", "[assets][png]") {
    const Image image = decodeImageFile(test::kTinyPng);
    REQUIRE(image.width == 2);
    REQUIRE(image.height == 2);
    REQUIRE(image.pixels.size() == 16);
    REQUIRE(image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    REQUIRE(image.pixel(1, 0) == Color::rgba(0, 255, 0, 255));
    REQUIRE(image.pixel(0, 1) == Color::rgba(0, 0, 255, 255));
    REQUIRE(image.pixel(1, 1) == Color::rgba(255, 255, 255, 0));
}

TEST_CASE("bytes that are not an image are rejected", "[assets][png]") {
    const std::vector<std::uint8_t> junk(64, 0x42);
    REQUIRE_THROWS_AS(decodeImageFile(junk), FormatError);
    REQUIRE_THROWS_AS(decodeImageFile(std::vector<std::uint8_t>{}), FormatError);
}

TEST_CASE("image files load from disk", "[assets][png]") {
    const auto dir = test::scratchDirectory("png-image");
    writeFile(dir / "tiny.png", test::kTinyPng);
    const Image image = loadImageFile(dir / "tiny.png");
    REQUIRE(image.width == 2);
    REQUIRE_THROWS_AS(loadImageFile(dir / "missing.png"), FileError);
}

TEST_CASE("the unpacked title backdrop is a 256 square", "[assets][png][unpacked]") {
    const auto file = test::unpackedOrSkip("TITLE/textures/011_TITLE00.png");
    const Image image = loadImageFile(file);
    REQUIRE(image.width == 256);
    REQUIRE(image.height == 256);
}

} // namespace
