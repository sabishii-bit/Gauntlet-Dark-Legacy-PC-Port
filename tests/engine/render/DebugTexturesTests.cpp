
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/DebugTextures.h"

namespace {

using namespace gdl;

Color pixelAt(const std::vector<u8>& pixels, u32 size, u32 x, u32 y) {
    const usize offset = (static_cast<usize>(y) * size + x) * 4;
    return Color::rgba(pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]);
}

TEST_CASE("checkerboard texture alternates cells starting with the even colour",
          "[render][textures]") {
    const Color even = Color::rgba(1, 2, 3, 4);
    const Color odd = Color::rgba(5, 6, 7, 8);
    const std::vector<u8> pixels = makeCheckerboardRgba8(8, 2, even, odd);

    REQUIRE(pixels.size() == 256);
    REQUIRE(pixelAt(pixels, 8, 0, 0) == even);
    REQUIRE(pixelAt(pixels, 8, 1, 1) == even);
    REQUIRE(pixelAt(pixels, 8, 2, 0) == odd);
    REQUIRE(pixelAt(pixels, 8, 0, 2) == odd);
    REQUIRE(pixelAt(pixels, 8, 2, 2) == even);
    REQUIRE(pixelAt(pixels, 8, 7, 7) == even);
}

TEST_CASE("a single-cell checkerboard is a solid colour", "[render][textures]") {
    const std::vector<u8> pixels = makeCheckerboardRgba8(4, 4, Color::black(), Color::white());
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            REQUIRE(pixelAt(pixels, 4, x, y) == Color::black());
        }
    }
}

} // namespace
