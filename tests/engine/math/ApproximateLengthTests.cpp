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

} // namespace
