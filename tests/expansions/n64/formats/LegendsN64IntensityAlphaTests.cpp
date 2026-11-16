#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/formats/LegendsN64IntensityAlpha.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

TEST_CASE("an N64 IA4 texel widens three bits of intensity over one of alpha",
          "[formats][legends][n64-intensity-alpha]") {
    for (u32 bits = 0; bits < 16; ++bits) {
        CAPTURE(bits);
        // Repeating the three bits rounds i * 255 / 7 to the nearest: 73 for 2, never 72.
        const auto grey = static_cast<u8>(std::lround((bits >> 1U) * 255.0 / 7));
        CHECK(legendsN64IntensityAlpha4(bits) ==
              Color::rgba(grey, grey, grey, (bits & 1U) != 0 ? 255 : 0));
    }
    CHECK(legendsN64IntensityAlpha4(0b0101) == Color::rgba(73, 73, 73, 255));
    // Full intensity may be clear and none may cover: the alpha is never the intensity.
    CHECK(legendsN64IntensityAlpha4(0b1110) == Color::rgba(255, 255, 255, 0));
    CHECK(legendsN64IntensityAlpha4(0b0001) == Color::rgba(0, 0, 0, 255));
}

TEST_CASE("an N64 IA8 texel is a nibble of intensity over a nibble of alpha",
          "[formats][legends][n64-intensity-alpha]") {
    for (u32 bits = 0; bits < 256; ++bits) {
        CAPTURE(bits);
        const auto grey = static_cast<u8>((bits >> 4U) * 17);
        CHECK(legendsN64IntensityAlpha8(bits) ==
              Color::rgba(grey, grey, grey, static_cast<u8>((bits & 15U) * 17)));
    }
    CHECK(legendsN64IntensityAlpha8(0x5A) == Color::rgba(85, 85, 85, 170));
    CHECK(legendsN64IntensityAlpha8(0xF0) == Color::rgba(255, 255, 255, 0));
}

} // namespace
