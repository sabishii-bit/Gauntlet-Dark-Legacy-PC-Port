#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"

namespace {

using namespace gdl;

/** The original's result for a shorter component `low` in the band adding `slope`. */
f32 banded(f64 high, f64 slope, f64 low) {
    return static_cast<f32>(high + slope * low);
}

TEST_CASE("approximateFlatLength ignores height and the components' signs", "[math]") {
    const f32 length = approximateFlatLength(Vec3{3.0f, 100.0f, -4.0f});
    CHECK(approximateFlatLength(Vec3{-3.0f, -7.0f, 4.0f}) == length);
    CHECK(approximateFlatLength(Vec3{4.0f, 0.0f, 3.0f}) == length);
    // A share of 3/4 lies on the sixth band's upper edge, which adds a third of it.
    CHECK(length == banded(4.0, 0.333, 3.0));
}

TEST_CASE("approximateFlatLength keeps a share on a band edge in the lower band", "[math]") {
    CHECK(approximateFlatLength(Vec3{8.0f, 0.0f, 1.0f}) == banded(8.0, 0.064, 1.0));
    CHECK(approximateFlatLength(Vec3{8.0f, 0.0f, 1.0001f}) == banded(8.0, 0.124, 1.0001f));
    CHECK(approximateFlatLength(Vec3{8.0f, 0.0f, 7.0f}) == banded(8.0, 0.376, 7.0));
    CHECK(approximateFlatLength(Vec3{8.0f, 0.0f, 7.0001f}) == banded(8.0, 0.414, 7.0001f));
    CHECK(approximateFlatLength(Vec3{5.0f, 0.0f, 5.0f}) == banded(5.0, 0.414, 5.0));
}

TEST_CASE("approximateFlatLength leaves a negligible component out, testing x first", "[math]") {
    CHECK(approximateFlatLength(Vec3{}) == 0.0f);
    CHECK(approximateFlatLength(Vec3{2.0f, 0.0f, 0.00005f}) == 2.0f);
    CHECK(approximateFlatLength(Vec3{0.00005f, 0.0f, 2.0f}) == 2.0f);
    // With both negligible the original returns |z| even when |x| is the longer.
    CHECK(approximateFlatLength(Vec3{0.00009f, 0.0f, -0.00001f}) == 0.00001f);
}

TEST_CASE("approximateFlatLength orders some near ties unlike the Euclidean length", "[math]") {
    const Vec3 diagonal{5.0f, 0.0f, 5.0f};
    const Vec3 alongX{7.06f, 0.0f, 0.3f};
    REQUIRE(std::hypot(alongX.x, alongX.z) < std::hypot(diagonal.x, diagonal.z));
    CHECK(approximateFlatLength(alongX) > approximateFlatLength(diagonal));
}

TEST_CASE("approximateLength measures along the ground first and then with the height", "[math]") {
    // The ground length is 3 and the height a third of it, which the third band takes.
    CHECK(approximateLength(Vec3{3.0f, 1.0f, 0.0f}) == banded(3.0, 0.181, 1.0));
    CHECK(approximateLength(Vec3{0.0f, -1.0f, 0.0f}) == 1.0f);
    CHECK(approximateLength(Vec3{}) == 0.0f);
    CHECK(approximateLength(Vec3{4.0f, 0.00005f, 3.0f}) ==
          approximateFlatLength(Vec3{4.0f, 0.0f, 3.0f}));
    // Measuring x with the height first would give about 4.642 here,
    CHECK(approximateLength(Vec3{1.0f, 2.0f, 4.0f}) ==
          banded(approximateFlatLength(Vec3{1.0f, 0.0f, 4.0f}), 0.236, 2.0));
    // and measuring either x or z with the height first about 4.596 here.
    CHECK(approximateLength(Vec3{1.0f, 4.0f, 2.0f}) ==
          banded(4.0, 0.287, approximateFlatLength(Vec3{1.0f, 0.0f, 2.0f})));
    // With the ground length and the height both negligible, the height, taken second, is left.
    CHECK(approximateLength(Vec3{0.00003f, 0.00002f, 0.0f}) == 0.00002f);
}

TEST_CASE("approximateUnit scales by the fast length's single-precision reciprocal and leaves "
          "no length alone",
          "[math]") {
    CHECK(approximateUnit(Vec3{1.0f, 0.0f, 0.0f}) == Vec3{1.0f, 0.0f, 0.0f});
    CHECK(approximateUnit(Vec3{}) == Vec3{0.0f});

    const Vec3 slope{3.0f, 1.0f, 0.0f};
    const f32 length = approximateLength(slope);
    const auto scale = static_cast<f32>(1.0 / static_cast<f64>(length));
    const Vec3 unit = approximateUnit(slope);
    CHECK(unit == Vec3{3.0f * scale, scale, 0.0f});
    // Dividing by the length in single precision rounds x differently.
    CHECK(unit.x != slope.x / length);
    // A negligible ground length leaves the height as the length scaled by.
    CHECK(approximateUnit(Vec3{0.00003f, 0.00002f, 0.0f}) == Vec3{1.5f, 1.0f, 0.0f});

    // The fast length of the diagonal, 1.414, falls just short of the Euclidean one.
    CHECK(glm::length(approximateUnit(Vec3{1.0f, 0.0f, 1.0f})) != 1.0f);
}

} // namespace
