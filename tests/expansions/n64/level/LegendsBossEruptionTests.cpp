#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "expansions/n64/level/LegendsBossEruption.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsMapObject fragment() {
    formats::LegendsMapObject object;
    object.name = "ROCKFLY_R12#0";
    object.flags = 0x200;
    object.transform = glm::translate(Mat4{1}, Vec3{10, 20, 30});
    return object;
}

void sameMatrix(const Mat4& left, const Mat4& right, f32 margin = 1e-5f) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(left[column][row] == Approx(right[column][row]).margin(margin));
        }
    }
}
} // namespace

TEST_CASE("Imported boss cap stays intact until its bounded one-shot eruption",
          "[legends][boss-eruption]") {
    const std::array objects{fragment()};
    LegendsBossEruption eruption{objects};
    REQUIRE(eruption.size() == 1);
    CHECK(eruption.visible(0) == true);
    CHECK_FALSE(eruption.visible(1));
    eruption.update(20);
    CHECK(eruption.visible(0) == true);
    CHECK(eruption.transform(0) == Mat4{1});
    REQUIRE(eruption.begin());
    CHECK(eruption.visible(0) == true);
    CHECK_FALSE(eruption.begin());
    for (const f32 seconds : {0.0f, -1.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        eruption.update(seconds);
        CHECK(eruption.transform(0) == Mat4{1});
    }
    eruption.capturePresentation();
    eruption.update(.25f);
    const auto pose = eruption.transform(0);
    CHECK(pose != Mat4{1});
    // Sampling and a paused (zero-time) tick cannot advance the visual clock.
    for (usize draw = 0; draw < 20; ++draw) {
        eruption.update(0);
        CHECK(eruption.transform(0) == pose);
    }
    CHECK_FALSE(eruption.begin());
    CHECK(eruption.transform(0) == pose);
    eruption.update(2.25f);
    CHECK(eruption.visible(0) == false);
    CHECK_FALSE(eruption.begin());
    eruption.update(std::numeric_limits<f32>::max());
    CHECK(eruption.visible(0) == false);
    CHECK(eruption.transform(1) == Mat4{1});
    CHECK(eruption.offset(1) == Vec3{0});

    LegendsBossEruption reloaded{objects};
    CHECK(reloaded.visible(0) == true);
    REQUIRE(reloaded.begin());
    CHECK(reloaded.transform(0) == Mat4{1});
    LegendsBossEruption absent;
    CHECK_FALSE(absent.begin());
    CHECK_FALSE(absent.visible(0));
}

TEST_CASE("Imported flying debris retains native gravity, spin and interpolation",
          "[legends][boss-eruption]") {
    const std::array objects{fragment()};
    LegendsBossEruption eruption{objects};
    REQUIRE(eruption.begin());
    eruption.capturePresentation();
    eruption.update(1.0f / 30);
    const Vec3 first = eruption.offset(0);
    REQUIRE(first.y >= 0);
    REQUIRE(first.y < 20.0f / 30);
    CHECK(first.x == 0);
    CHECK(first.z == 0);
    CHECK(eruption.offset(0, 0) == Vec3{0});
    CHECK(eruption.offset(0, .5f).y == Approx(first.y / 2));
    eruption.capturePresentation();
    eruption.update(1.0f / 30);
    CHECK(eruption.offset(0).y - first.y * 2 == Approx(-64.0f / 900).margin(.00001f));
    for (const f32 alpha : {0.0f, .25f, .5f, 1.0f}) {
        const Mat4 pose = eruption.transform(0, alpha);
        const Vec3 anchor{objects[0].transform[3]};
        CHECK(glm::distance(Vec3{pose * Vec4{anchor, 1}}, anchor + eruption.offset(0, alpha)) <
              .00001f);
        CHECK(glm::determinant(Mat3{pose}) == Approx(1).margin(.00001f));
        CHECK(glm::length(Vec3{pose[0]}) == Approx(1).margin(.00001f));
    }
}

TEST_CASE("Imported eruption is independent of host frame rate", "[legends][boss-eruption]") {
    const std::array objects{fragment()};
    LegendsBossEruption reference{objects};
    REQUIRE(reference.begin());
    reference.update(2);
    for (const s32 rate : {30, 60, 144}) {
        CAPTURE(rate);
        LegendsBossEruption candidate{objects};
        REQUIRE(candidate.begin());
        for (s32 frame = 0; frame < rate * 2; ++frame) {
            candidate.capturePresentation();
            candidate.update(1.0f / static_cast<f32>(rate));
        }
        CHECK(candidate.visible(0) == reference.visible(0));
        sameMatrix(candidate.transform(0), reference.transform(0), .0001f);
    }
}

TEST_CASE("Imported eruption never recruits an intact floor or unbounded foreign roster",
          "[legends][boss-eruption]") {
    std::vector<formats::LegendsMapObject> objects(3, fragment());
    objects[0].name = "FLOOR__FL#0";
    objects[1].flags = 0;
    objects[2].name = "ROCKFLY_R12#1";
    LegendsBossEruption eruption{objects};
    CHECK(eruption.size() == 1);
    CHECK_FALSE(eruption.visible(0));
    CHECK_FALSE(eruption.visible(1));
    CHECK(eruption.visible(2) == true);
    objects.assign(LegendsBossEruption::kMaximumFragments, fragment());
    REQUIRE_NOTHROW(LegendsBossEruption{objects});
    objects.push_back(fragment());
    REQUIRE_THROWS_AS(LegendsBossEruption{objects}, FormatError);
    objects.resize(1);
    objects[0].transform[3].x = std::numeric_limits<f32>::quiet_NaN();
    REQUIRE_THROWS_AS(LegendsBossEruption{objects}, FormatError);
}
