#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/party/BodyCollision.h"

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

TEST_CASE("item movement keeps box corners and height instead of substituting a round body",
          "[game][collision][generator-collision]") {
    std::array items{Obstacle{.centre = {0, 0, 5}, .halfAcross = 3, .halfAlong = 1, .height = 2}};
    const Vec3 stop = BodyCollision::resolveItems({0, 0, 0}, {0, 0, 20}, 1, 6, items);
    CHECK(stop == Vec3{0, 0, 3});
    CHECK(BodyCollision::resolveItems({0, 3, 0}, {0, 3, 20}, 1, 6, items) == Vec3{0, 3, 20});
    CHECK(BodyCollision::resolveItems({0, -7, 0}, {0, -7, 20}, 1, 6, items) == Vec3{0, -7, 20});
    CHECK(BodyCollision::resolveItems({0, -4, 0}, {0, -4, 20}, 1, 6, items) == Vec3{0, -4, 3});
    // Along the long side, outside a circle of the short radius, remains blocked.
    CHECK(BodyCollision::resolveItems({2.5f, 0, 0}, {2.5f, 0, 20}, 1, 6, items).z == Approx(3));
    CHECK(BodyCollision::resolveItems(stop, {2, 0, 3}, 1, 6, items) == Vec3{2, 0, 3});
    CHECK(BodyCollision::resolveItems(stop, {0, 0, 0}, 1, 6, items) == Vec3{0, 0, 0});
    items[0].solid = false;
    CHECK(BodyCollision::resolveItems({0, 0, 0}, {0, 0, 20}, 1, 6, items) == Vec3{0, 0, 20});
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

TEST_CASE("a contact cylinder holds the collision centre at its radius and only within its height",
          "[game][collision][critter-contact]") {
    // A part two and a half wide and five high, met by a width of one and a half
    // (CritterExpNodeColSub): four across, and four up or down from its middle at y 4.
    const auto cylinder =
        BodyCollision::critterContact(MissileTarget{0, {0, 1.5f, 10}, 2.5f, 5}, 1.5f);
    CHECK(cylinder.centre == Vec3{0, 4, 10});
    CHECK(cylinder.radius == 4);
    CHECK(cylinder.halfHeight == 4);
    const std::array cylinders{cylinder};
    constexpr f32 kCentre = 2.5f; ///< the collision centre over the feet
    const auto walk = [&](f32 feet) {
        return BodyCollision::resolve({0, feet, 0}, {0, feet, 20}, 0.75f, 5, {}, kCentre,
                                      cylinders);
    };
    const Vec3 stop = walk(0);
    CHECK(stop.x == 0);
    CHECK(stop.z == Approx(6));
    // The band's edges still hold the centre; just past them it walks on.
    CHECK(walk(5.5f).z == Approx(6));
    CHECK(walk(-2.5f).z == Approx(6));
    CHECK(walk(5.51f) == Vec3{0, 5.51f, 20});
    CHECK(walk(-2.51f) == Vec3{0, -2.51f, 20});
}

TEST_CASE("bodies and contact cylinders share one sweep", "[game][collision][critter-contact]") {
    const std::array bodies{MissileTarget{0, {-2, 0, 5}, 2, 6}};
    const std::array cylinders{
        BodyCollision::critterContact(MissileTarget{1, {2, 0, 5}, 1.5f, 6}, 1.5f)};
    REQUIRE(cylinders.front().radius == 3);
    const Vec3 stop = BodyCollision::resolve({0, 0, 0}, {0, 0, 20}, 1, 6, bodies, 2.5f, cylinders);
    CHECK(stop.z < 5);
    CHECK(glm::distance(Vec2{stop.x, stop.z}, Vec2{-2, 5}) >= 3 - 1e-4f);
    CHECK(glm::distance(Vec2{stop.x, stop.z}, Vec2{2, 5}) >= 3 - 1e-4f);
    // Without cylinders the sweep is the bodies' own.
    CHECK(BodyCollision::resolve({0, 0, 0}, {3, 0, 20}, 1, 6, bodies, 2.5f, {}) ==
          BodyCollision::resolve({0, 0, 0}, {3, 0, 20}, 1, 6, bodies));
}
} // namespace
