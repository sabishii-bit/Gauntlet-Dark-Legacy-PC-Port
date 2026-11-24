#include <algorithm>
#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/level/ItemFigure.h"
#include "game/party/BodyCollision.h"
#include "game/players/PlayerActor.h"

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

TEST_CASE("a player's step is held a whole width off a generator's box",
          "[game][collision][generator-collision][item-width]") {
    // PlayerCollideEnemies meets an item's box within col_radius, the class's whole width, and
    // puts the step out to it (items.c 1903-1913, 2036-2060; pmotion.c 1124-1135).
    PlayerActor player;
    player.spawn(0, {}, nullptr, Vec3{0}, 0);
    REQUIRE(player.reach() == 1.5f);
    const std::array generators{
        Obstacle{.centre = {0, 0, 5}, .halfAcross = 2, .halfAlong = 1, .height = 6}};
    // With a world under the step too, which runs the recheck of its walls and the boxes.
    WorldCollision floor;
    CollisionTriangle first;
    first.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}};
    first.normal = {0, 1, 0};
    CollisionTriangle second = first;
    second.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}};
    floor.build({first, second});
    for (const WorldCollision* world : std::array<const WorldCollision*, 2>{nullptr, &floor}) {
        CAPTURE(world != nullptr);
        const Vec3 stop =
            BodyCollision::resolveMovement(player, {0, 0, 0}, {0, 0, 4}, {}, generators, world);
        CHECK(stop.x == 0);
        CHECK(stop.z == Approx(4 - player.reach()));
        // Held there, a step along the box goes on, and one back off it goes freely.
        const Vec3 along = BodyCollision::resolveMovement(player, stop, stop + Vec3{0.5f, 0, 0}, {},
                                                          generators, world);
        CHECK(along.x == Approx(0.5f));
        CHECK(along.z == Approx(stop.z));
        const Vec3 back =
            BodyCollision::resolveMovement(player, stop, Vec3{0, 0, 0}, {}, generators, world);
        CHECK(glm::length(back) < 1e-5f);
    }
}

TEST_CASE("a step is trimmed toward a point out of what it met, axis by axis, as "
          "PlayerCollideWalls trims it",
          "[game][collision][party-spacing]") {
    // pmotion.c 3262-3302: an axis on which the point lies back against the step gives up that
    // much of it, at most all; any other axis of the step goes, and the way to the point along
    // it is left for the push velocity (light_vel, times gClockFrameReciprocal).
    // Out ahead along the step: the step goes, and the way out is left over.
    auto trim = BodyCollision::trimToward({0, 0, -3.2f}, {0, 0, -2.9f}, {0, 0, -2.5f});
    CHECK(trim.to == Vec3{0, 0, -3.2f});
    CHECK(trim.away.x == 0.0f);
    CHECK(trim.away.z == Approx(0.4f));
    // Back against it: the step gives up that much.
    trim = BodyCollision::trimToward({0, 0, -2.6f}, {0, 0, -2.3f}, {0, 0, -2.5f});
    CHECK(trim.to.x == 0.0f);
    CHECK(trim.to.z == Approx(-2.5f));
    CHECK(trim.away == Vec3{0});
    // Each axis on its own: the way out sideways is left over, the way back trimmed.
    trim = BodyCollision::trimToward({0.5f, 0, -2.6f}, {0.5f, 0, -2.2f}, {0.55261f, 0, -2.43148f});
    CHECK(trim.to.x == 0.5f);
    CHECK(trim.to.z == Approx(-2.43148f));
    CHECK(trim.away.x == Approx(0.05261f).epsilon(1e-4));
    CHECK(trim.away.z == 0.0f);
    // Never more than all of the step.
    trim = BodyCollision::trimToward({0, 0, 0}, {0, 0, 0.3f}, {0, 0, -0.5f});
    CHECK(trim.to == Vec3{0});
    CHECK(trim.away == Vec3{0});
    // The step keeps its height, and the way out is along the ground.
    trim = BodyCollision::trimToward({0, 1, -3.2f}, {0, 1.5f, -2.9f}, {0, 7, -2.5f});
    CHECK(trim.to == Vec3{0, 1.5f, -3.2f});
    CHECK(trim.away.y == 0.0f);
    CHECK(trim.away.z == Approx(0.4f));
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

TEST_CASE("a slide resting against a body goes on round it", "[game][collision]") {
    // A warrior's footprint walking north in quarter steps past a grunt's: a step that ends a
    // rounding inside the reach slides on along it, rather than meeting it again where it stands.
    const std::array bodies{MissileTarget{0, {0, 0, 0}, 1.5f, 6}};
    Vec3 at{0.6f, 0, -6};
    f32 nearest = glm::length(Vec2{at.x, at.z});
    for (s32 step = 0; step < 64; ++step) {
        const Vec3 from = at;
        at = BodyCollision::resolve(from, from + Vec3{0, 0, 0.25f}, 0.75f, 5, bodies);
        nearest = std::min(nearest, glm::length(Vec2{at.x, at.z}));
        CAPTURE(step, at.x, at.z);
        REQUIRE(glm::distance(at, from) > 0.05f);
    }
    CHECK(nearest >= 2.25f - 1e-4f);
    CHECK(at.z > 2.25f);
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

TEST_CASE("a step meets a contact cylinder as the original's line-cylinder test does",
          "[game][collision][auto-attack-stop]") {
    // Four across and four up or down about (0, 4, 10), as in the case above.
    const std::array cylinders{
        BodyCollision::critterContact(MissileTarget{0, {0, 1.5f, 10}, 2.5f, 5}, 1.5f)};
    const auto meets = [&](const Vec3& from, const Vec3& to) {
        return BodyCollision::touches(from, to, cylinders);
    };
    // Head on, a step ending at the radius meets it and one ending short of it does not.
    CHECK(meets({0, 4, 0}, {0, 4, 6.5f}));
    CHECK(meets({0, 4, 0}, {0, 4, 6}));
    CHECK_FALSE(meets({0, 4, 0}, {0, 4, 5.99f}));
    // From inside, a step along the body still meets it, as does one leaning away from it by
    // less than LineCylinderCollide's hundredth (combat.c 568-573); one leaning further away, or
    // heading straight out, is let go.
    CHECK(meets({0, 4, 7}, {1, 4, 7}));
    CHECK(meets({0, 4, 7}, {1, 4, 6.995f}));
    CHECK_FALSE(meets({0, 4, 7}, {1, 4, 6.98f}));
    CHECK_FALSE(meets({0, 4, 7}, {0, 4, 6.5f}));
    // A step passing by clear of it, or grazing it just outside the radius, does not meet it;
    // one passing just inside does.
    CHECK_FALSE(meets({-6, 4, 0}, {-6, 4, 20}));
    CHECK_FALSE(meets({-4.01f, 4, 0}, {-4.01f, 4, 20}));
    CHECK(meets({-3.99f, 4, 0}, {-3.99f, 4, 20}));
    // Over the band or under it the step passes; at its edges it still meets it.
    CHECK_FALSE(meets({0, 8.01f, 0}, {0, 8.01f, 20}));
    CHECK_FALSE(meets({0, -0.01f, 0}, {0, -0.01f, 20}));
    CHECK(meets({0, 8, 0}, {0, 8, 20}));
    CHECK(meets({0, 0, 0}, {0, 0, 20}));
    // Any one of several is met; none, nothing is.
    const std::array pair{cylinders.front(), BodyCollision::critterContact(
                                                 MissileTarget{1, {10, 1.5f, 10}, 2.5f, 5}, 1.5f)};
    CHECK(BodyCollision::touches({10, 4, 0}, {10, 4, 6.5f}, pair));
    CHECK_FALSE(BodyCollision::touches({5, 4, 0}, {5, 4, 20}, pair));
    CHECK_FALSE(BodyCollision::touches({0, 4, 0}, {0, 4, 20}, {}));
}
} // namespace
