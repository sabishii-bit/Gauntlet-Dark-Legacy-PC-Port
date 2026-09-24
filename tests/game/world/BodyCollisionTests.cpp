#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/BodyCollision.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("body movement sweeps before contact instead of tunnelling through",
          "[game][collision]") {
    const std::array bodies{MissileTarget{0, {0, 0, 5}, 2, 6}};
    const Vec3 stop = BodyCollision::resolve({0, 0, 0}, {0, 0, 20}, 1, 6, bodies);
    CHECK(stop.z == Approx(2));
    CHECK(stop.x == 0);
    CHECK(BodyCollision::resolve({0, 7, 0}, {0, 7, 20}, 1, 6, bodies) == Vec3{0, 7, 20});
    CHECK(BodyCollision::resolve({0, -8, 0}, {0, -8, 20}, 1, 6, bodies) == Vec3{0, -8, 20});
}

TEST_CASE("body movement slides and lets an overlapping spawn escape", "[game][collision]") {
    const std::array bodies{MissileTarget{0, {0, 0, 5}, 2, 6}};
    const Vec3 slide = BodyCollision::resolve({0, 0, 0}, {4, 0, 10}, 1, 6, bodies);
    CHECK(slide.x > 0);
    CHECK(glm::distance(Vec2{slide.x, slide.z}, Vec2{0, 5}) >= 3 - 1e-5f);
    CHECK(BodyCollision::resolve({0, 0, 3}, {0, 0, 1}, 1, 6, bodies) == Vec3{0, 0, 1});
    CHECK(BodyCollision::resolve({0, 0, 3}, {0, 0, 4}, 1, 6, bodies) == Vec3{0, 0, 3});
    CHECK(BodyCollision::resolve({0, 0, 5}, {0, 0, 6}, 1, 6, bodies) == Vec3{0, 0, 6});
}

TEST_CASE("a creature slide cannot tunnel through a neighbouring creature", "[game][collision]") {
    const std::array bodies{MissileTarget{0, {-2, 0, 5}, 2, 6}, MissileTarget{1, {2, 0, 5}, 2, 6}};
    const Vec3 stop = BodyCollision::resolve({0, 0, 0}, {0, 0, 20}, 1, 6, bodies);
    CHECK(stop.z < 5);
    for (const auto& body : bodies) {
        CHECK(glm::distance(Vec2{stop.x, stop.z}, Vec2{body.base.x, body.base.z}) >= 3 - 1e-4f);
    }
}
} // namespace
