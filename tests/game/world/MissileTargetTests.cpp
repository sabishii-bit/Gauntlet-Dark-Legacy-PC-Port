#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/MoveStrikes.h"
#include "game/world/PlayerMissiles.h"
#include "game/world/TargetAssist.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Mesh targets use triangle faces and edges rather than enclosing cylinders",
          "[walls][missile-target]") {
    CollisionTriangle triangle;
    triangle.normal = {0, 0, -1};
    triangle.vertices = {Vec3{-5, 0, 5}, Vec3{5, 0, 5}, Vec3{0, 10, 5}};
    const std::array surface{triangle};
    const MissileTarget target{6000, {0, 0, 5}, 5, 10, surface};
    CHECK(target.pointNear({0, 3, 0}) == Vec3{0, 3, 5});
    CHECK(target.touches({0, 3, 4.8f}, 0.25f));
    CHECK_FALSE(target.touches({0, 3, 0}, 1));
    CHECK_FALSE(target.touches({4, 9, 5}, 0.25f));
    CHECK(target.touches({0, 10.2f, 5}, 0.25f));
    CHECK_FALSE(target.touches({0, -2, 5}, 1));

    StrikeHit strike;
    strike.centre = {0, 3, 4};
    strike.radius = 1.1f;
    strike.damage = 10;
    CHECK(target.reachedBy(strike));
    strike.centre = {0, 3, 0};
    CHECK_FALSE(target.reachedBy(strike));
    strike.swept = true;
    strike.from = {0, 3, 0};
    strike.centre = {0, 3, 10};
    CHECK(target.reachedBy(strike));
    strike.from.x = strike.centre.x = 20;
    CHECK_FALSE(target.reachedBy(strike));

    WorldCollision collision;
    collision.build({triangle});
    const std::array targets{target};
    CHECK(TargetAssist::around({0, 0, 3}, 6, targets, 3, &collision));
    CHECK_FALSE(TargetAssist::around({0, 0, 0}, 6, targets, 3, &collision));
    CHECK(TargetAssist::select({0, 3, 0}, {0, 0, 1}, targets, 30, &collision));

    // An ordinary wall in front still obstructs aiming at the destructible one.
    for (auto& v : triangle.vertices) {
        v.z = 2;
    }
    collision.append(std::span{&triangle, 1});
    CHECK_FALSE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, targets, 30, &collision));
}

TEST_CASE("Projectiles report a mesh target instead of disappearing against its collision",
          "[walls][missile-target]") {
    CollisionTriangle triangle;
    triangle.vertices = {Vec3{-5, 0, 5}, Vec3{5, 0, 5}, Vec3{0, 10, 5}};
    triangle.normal = {0, 0, 1};
    const std::array surface{triangle};
    const std::array targets{MissileTarget{6000, {0, 0, 5}, 5, 10, surface}};
    WorldCollision collision;
    collision.build({triangle});
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    MissileLaunch launch;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 20};
    launch.spec = &spec;
    launch.damage = 10;
    PlayerMissiles missiles;
    REQUIRE(missiles.launch(launch));
    missiles.update(0.5f, &collision, targets);
    const auto impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts[0].target == 6000);
    CHECK(impacts[0].damage == Catch::Approx(10));
}

TEST_CASE("narrow area-cover probes cannot step through a mesh between sample points",
          "[walls][missile-target][multiplayer-combat]") {
    CollisionTriangle triangle;
    triangle.vertices = {Vec3{-5, 0, 5}, Vec3{5, 0, 5}, Vec3{0, 10, 5}};
    triangle.normal = {0, 0, 1};
    const std::array surface{triangle};
    const MissileTarget target{6000, {0, 0, 5}, 5, 10, surface};
    StrikeHit probe;
    probe.radius = 0.1f;
    probe.damage = 1;
    probe.from = {0, 2, 0};
    probe.centre = {0, 2.5f, 10};
    probe.swept = true;
    // At fixed quarter-unit sampling this slight rise makes 41 intervals: both
    // samples beside the wall miss it by more than this probe's tenth-unit radius.
    CHECK(target.reachedBy(probe));
    probe.from.x = probe.centre.x = 10;
    CHECK_FALSE(target.reachedBy(probe));
}
} // namespace
