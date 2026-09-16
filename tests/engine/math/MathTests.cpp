#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace {

using namespace gdl;
using Catch::Matchers::WithinAbs;

constexpr f64 kEpsilon = 1e-5;

TEST_CASE("angle conversions round-trip", "[math]") {
    CHECK_THAT(degreesToRadians(180.0f), WithinAbs(kPi, kEpsilon));
    CHECK_THAT(radiansToDegrees(kHalfPi), WithinAbs(90.0, kEpsilon));
    CHECK_THAT(radiansToDegrees(degreesToRadians(37.5f)), WithinAbs(37.5, kEpsilon));
    STATIC_REQUIRE(kTwoPi == 2.0f * kPi);
}

TEST_CASE("Rect edges derive from origin and size", "[math]") {
    constexpr Rect kRect{10.0f, 20.0f, 30.0f, 40.0f};
    STATIC_REQUIRE(kRect.right() == 40.0f);
    STATIC_REQUIRE(kRect.bottom() == 60.0f);
    REQUIRE(kRect == Rect{10.0f, 20.0f, 30.0f, 40.0f});
}

TEST_CASE("Color is stored as R, G, B, A bytes", "[math][color]") {
    const Color color = Color::rgba(1, 2, 3, 4);
    std::array<u8, 4> bytes{};
    std::memcpy(bytes.data(), &color, sizeof(color));
    REQUIRE(bytes == std::array<u8, 4>{1, 2, 3, 4});
}

TEST_CASE("Color::fromFloats rounds and clamps", "[math][color]") {
    REQUIRE(Color::fromFloats(1.0f, 0.0f, 0.5f) == Color::rgba(255, 0, 128));
    REQUIRE(Color::fromFloats(2.0f, -1.0f, 0.25f, -0.5f) == Color::rgba(255, 0, 64, 0));
    REQUIRE(Color::fromFloats(0.0f, 0.0f, 0.0f, 0.0f) == Color::transparent());
}

TEST_CASE("Color helpers", "[math][color]") {
    REQUIRE(Color::white() == Color::rgba(255, 255, 255, 255));
    REQUIRE(Color::black() == Color::rgba(0, 0, 0, 255));
    REQUIRE(Color::rgba(9, 8, 7).withAlpha(100) == Color::rgba(9, 8, 7, 100));
    const Vec4 v = Color::rgba(255, 0, 51, 102).toVec4();
    CHECK_THAT(v.r, WithinAbs(1.0, kEpsilon));
    CHECK_THAT(v.g, WithinAbs(0.0, kEpsilon));
    CHECK_THAT(v.b, WithinAbs(0.2, kEpsilon));
    CHECK_THAT(v.a, WithinAbs(0.4, kEpsilon));
}

TEST_CASE("makeScreenProjection maps pixel corners to Vulkan clip space", "[math][projection]") {
    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);

    const Vec4 topLeft = projection * Vec4{0.0f, 0.0f, 0.25f, 1.0f};
    CHECK_THAT(topLeft.x, WithinAbs(-1.0, kEpsilon));
    CHECK_THAT(topLeft.y, WithinAbs(-1.0, kEpsilon));
    CHECK_THAT(topLeft.z, WithinAbs(0.25, kEpsilon));
    CHECK_THAT(topLeft.w, WithinAbs(1.0, kEpsilon));

    const Vec4 bottomRight = projection * Vec4{640.0f, 448.0f, 1.0f, 1.0f};
    CHECK_THAT(bottomRight.x, WithinAbs(1.0, kEpsilon));
    CHECK_THAT(bottomRight.y, WithinAbs(1.0, kEpsilon));
    CHECK_THAT(bottomRight.z, WithinAbs(1.0, kEpsilon));
}

TEST_CASE("fitFrame letterboxes to the limiting dimension", "[math][projection]") {
    SECTION("wider target: bars on the sides") {
        const Letterbox fit = fitFrame(640.0f, 448.0f, 2560.0f, 896.0f);
        REQUIRE(fit == Letterbox{2.0f, 640.0f, 0.0f});
    }
    SECTION("taller target: bars above and below") {
        const Letterbox fit = fitFrame(640.0f, 448.0f, 640.0f, 896.0f);
        REQUIRE(fit == Letterbox{1.0f, 0.0f, 224.0f});
    }
    SECTION("exact aspect: no bars") {
        const Letterbox fit = fitFrame(640.0f, 448.0f, 1280.0f, 896.0f);
        REQUIRE(fit == Letterbox{2.0f, 0.0f, 0.0f});
    }
}

TEST_CASE("makeLetterboxProjection places the frame inside the target", "[math][projection]") {
    const Mat4 projection = makeLetterboxProjection(640.0f, 448.0f, 2560.0f, 896.0f);

    const Vec4 frameTopLeft = projection * Vec4{0.0f, 0.0f, 0.5f, 1.0f};
    CHECK_THAT(frameTopLeft.x, WithinAbs(-0.5, kEpsilon));
    CHECK_THAT(frameTopLeft.y, WithinAbs(-1.0, kEpsilon));
    CHECK_THAT(frameTopLeft.z, WithinAbs(0.5, kEpsilon));

    const Vec4 frameBottomRight = projection * Vec4{640.0f, 448.0f, 0.5f, 1.0f};
    CHECK_THAT(frameBottomRight.x, WithinAbs(0.5, kEpsilon));
    CHECK_THAT(frameBottomRight.y, WithinAbs(1.0, kEpsilon));
}

} // namespace
