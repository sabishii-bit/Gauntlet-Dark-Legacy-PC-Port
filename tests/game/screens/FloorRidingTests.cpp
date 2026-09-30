#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/screens/FloorRiding.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr u32 kFloorQuery = 0x4; ///< of WorldCollision::kFloorQueryFlags

/** A square of floor from `min` to `max` at height `y`, belonging to `object`. */
std::vector<CollisionTriangle> square(Vec2 min, Vec2 max, f32 y, s32 object, u32 flags) {
    CollisionTriangle first;
    first.vertices = {Vec3{min.x, y, min.y}, Vec3{max.x, y, max.y}, Vec3{max.x, y, min.y}};
    first.object = object;
    first.objectFlags = flags;
    CollisionTriangle second = first;
    second.vertices = {Vec3{min.x, y, min.y}, Vec3{min.x, y, max.y}, Vec3{max.x, y, max.y}};
    return {first, second};
}

TEST_CASE("a member rides a moving floor, turned with it", "[game][screens][floor-riding]") {
    constexpr s32 kTurntable = 7;
    WorldCollision collision;
    collision.build(square({-5, -5}, {5, 5}, 0, kTurntable, FloorRiding::kMoving | kFloorQuery));
    const std::array<s32, 1> moving{kTurntable};
    collision.setMovingObjects(moving);
    collision.setObjectTransform(kTurntable, Mat4{1.0f});
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{2, 0, 0}, 0.0f);
    FloorRiding::land(players, 0, players[0].actor.position(), collision);
    REQUIRE(players[0].floor.object == kTurntable);
    REQUIRE(players[0].floor.placement.has_value());

    // A quarter turn and a lift of one: the body goes round and up, facing round with it.
    constexpr f32 kQuarter = std::numbers::pi_v<f32> / 2.0f;
    collision.setObjectTransform(kTurntable, glm::rotate(glm::translate(Mat4{1.0f}, Vec3{0, 1, 0}),
                                                         kQuarter, Vec3{0, 1, 0}));
    FloorRiding::carry(players[0], collision);
    const Vec3 at = players[0].actor.position();
    CHECK(at.x == Approx(0.0f).margin(1e-5f));
    CHECK(at.y == Approx(1.0f));
    CHECK(at.z == Approx(-2.0f));
    CHECK(std::remainder(players[0].actor.yaw() - kQuarter, 2.0f * std::numbers::pi_v<f32>) ==
          Approx(0.0f).margin(1e-5f));
    // Carried once, it is not carried again until the floor moves on.
    FloorRiding::carry(players[0], collision);
    CHECK(players[0].actor.position().z == Approx(-2.0f));
}

TEST_CASE("a still floor carries nothing, and neither does one left behind",
          "[game][screens][floor-riding]") {
    WorldCollision collision;
    collision.build(square({-5, -5}, {5, 5}, 0, 3, kFloorQuery));
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{1, 0, 1}, 0.0f);
    FloorRiding::land(players, 0, players[0].actor.position(), collision);
    CHECK(players[0].floor.object == 3);
    CHECK_FALSE(players[0].floor.placement.has_value());
    FloorRiding::carry(players[0], collision);
    CHECK(players[0].actor.position() == Vec3{1, 0, 1});
}

TEST_CASE("a member may not step onto a moving floor while another rides one kept apart",
          "[game][screens][floor-riding]") {
    constexpr s32 kLift = 7;
    constexpr s32 kOtherLift = 8;
    WorldCollision collision;
    std::vector<CollisionTriangle> triangles = square({-20, -20}, {20, -5}, 0, 1, kFloorQuery);
    for (const auto& triangle :
         square({-5, -5}, {5, 5}, 0, kLift, FloorRiding::kMoving | kFloorQuery)) {
        triangles.push_back(triangle);
    }
    for (const auto& triangle :
         square({10, -5}, {20, 5}, 0, kOtherLift,
                FloorRiding::kMoving | FloorRiding::kKeptApart | kFloorQuery)) {
        triangles.push_back(triangle);
    }
    collision.build(triangles);
    const std::array<s32, 2> moving{kLift, kOtherLift};
    collision.setMovingObjects(moving);
    collision.setObjectTransform(kLift, Mat4{1.0f});
    collision.setObjectTransform(kOtherLift, Mat4{1.0f});
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, -6}, 0.0f);
    players[1].actor.spawn(1, {}, nullptr, Vec3{15, 0, 0}, 0.0f);
    FloorRiding::land(players, 1, players[1].actor.position(), collision);
    REQUIRE(players[1].floor.object == kOtherLift);
    // The step onto the lift is refused while the other rides theirs.
    const Vec3 from = players[0].actor.position();
    players[0].actor.place(Vec3{0, 0, -4});
    FloorRiding::land(players, 0, from, collision);
    CHECK(players[0].actor.position().z == Approx(-6.0f));
    CHECK(players[0].floor.object == 1);
    // Off theirs, the way is open.
    players[1].life = PlayerLife::InTower;
    players[0].actor.place(Vec3{0, 0, -4});
    FloorRiding::land(players, 0, from, collision);
    CHECK(players[0].actor.position().z == Approx(-4.0f));
    CHECK(players[0].floor.object == kLift);
}

TEST_CASE("a travelling lift keeps riders aboard without freezing movement on its deck",
          "[game][screens][floor-riding]") {
    WorldCollision collision;
    auto triangles = square({-5, -5}, {0, 5}, 0, 7, kFloorQuery);
    const auto adjacent = square({0, -5}, {5, 5}, 0, 8, kFloorQuery);
    triangles.insert(triangles.end(), adjacent.begin(), adjacent.end());
    collision.build(triangles);
    std::array<PlayerRuntime, 1> players;
    auto& actor = players[0].actor;
    actor.spawn(0, {}, nullptr, Vec3{-2, 0, 0}, 0);
    FloorRiding::land(players, 0, actor.position(), collision);
    REQUIRE(players[0].floor.object == 7);
    collision.setFloorExitBlocked(7, true);
    collision.setFloorExitBlocked(7, true); // idempotent
    actor.place(Vec3{-1, 0, 0});
    FloorRiding::land(players, 0, Vec3{-2, 0, 0}, collision);
    CHECK(actor.position() == Vec3{-1, 0, 0});
    const Vec3 from = actor.position();
    SECTION("a different floor") {
        actor.place(Vec3{1, 0, 0});
    }
    SECTION("no floor") {
        actor.place(Vec3{-6, 0, 0});
    }
    FloorRiding::land(players, 0, from, collision);
    CHECK(actor.position() == from);
    CHECK(players[0].floor.object == 7);
    collision.setFloorExitBlocked(7, false);
    actor.place(Vec3{1, 0, 0});
    FloorRiding::land(players, 0, from, collision);
    CHECK(actor.position() == Vec3{1, 0, 0});
    CHECK(players[0].floor.object == 8);
    collision.setFloorExitBlocked(7, true);
    collision.setSolid(7, false);
    CHECK_FALSE(collision.floorExitBlocked(7));
    collision.setSolid(7, true);
    CHECK(collision.floorExitBlocked(7));
    collision.clear();
    CHECK_FALSE(collision.floorExitBlocked(7));
}

} // namespace
