#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/TargetAssist.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("target assist chooses the nearest forward surface without targeting behind",
          "[game][target-assist]") {
    const Vec3 origin{0, 3, 0};
    const Vec3 facing{0, 0, 1};
    const std::array targets{MissileTarget{0, {3, 0, 10}, 1, 6}, MissileTarget{1, {0, 0, -1}, 1, 6},
                             MissileTarget{2, {8, 0, 2}, 1, 6}};
    REQUIRE(TargetAssist::select(origin, facing, targets, 30) == Vec3{3, 3, 10});
    REQUIRE_FALSE(TargetAssist::select(origin, facing, targets, 5));
    REQUIRE_FALSE(TargetAssist::select(origin, Vec3{0}, targets, 30));
    REQUIRE_FALSE(TargetAssist::select(origin, facing, {}, 30));
    const std::array large{targets[0], MissileTarget{3, {0, 0, 14}, 6, 6}};
    REQUIRE(TargetAssist::select(origin, facing, large, 30) == Vec3{0, 3, 14});
}

TEST_CASE("target assist extends range in boss encounters and rejects blocked targets",
          "[game][target-assist]") {
    const std::array target{MissileTarget{0, {0, 0, 60}, 4, 12}};
    REQUIRE_FALSE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kRange));
    REQUIRE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kBossRange));
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 10}, Vec3{10, 0, 10}, Vec3{0, 20, 10}}}});
    REQUIRE_FALSE(
        TargetAssist::select({0, 3, 0}, {0, 0, 1}, target, TargetAssist::kBossRange, &collision));
}

TEST_CASE("assisted shots reach raised and lowered targets without changing horizontal pace",
          "[game][target-assist]") {
    const Vec3 origin{0, 5, 0};
    for (const f32 height : {-3.0f, 5.0f, 20.0f}) {
        const Vec3 target{6, height, 8};
        const Vec3 velocity = TargetAssist::velocity(origin, target, 20, 8);
        REQUIRE(std::hypot(velocity.x, velocity.z) == Approx(20));
        const f32 time = 0.5f;
        const Vec3 landed = origin + velocity * time - Vec3{0, 4 * time * time, 0};
        REQUIRE(glm::distance(landed, target) == Approx(0).margin(1e-5f));
    }
    REQUIRE(TargetAssist::velocity(origin, origin, 20, 8) == Vec3{0});
}
TEST_CASE("melee reach uses horizontal surfaces with vertical overlap and wall visibility",
          "[game][target-assist][melee]") {
    std::array targets{MissileTarget{0, {0, 0, 5}, 3, 18}, MissileTarget{1, {0, 10, 1}, 1, 4},
                       MissileTarget{2, {0, 0, -2}, 1, 4}};
    const auto selected = TargetAssist::melee({0, 0, 0}, 6, {0, 0, 1}, targets, 3);
    REQUIRE(selected);
    CHECK(selected->id == 0);
    CHECK_FALSE(TargetAssist::melee({0, 0, 0}, 6, {0, 0, 1}, targets, 1));
    targets[0] = {0, {0, 0, 5}, 1, 6};
    WorldCollision collision;
    collision.build({{{0, 0, -1}, {Vec3{-10, 0, 2}, Vec3{10, 0, 2}, Vec3{0, 20, 2}}}});
    CHECK_FALSE(TargetAssist::melee({0, 0, 0}, 6, {0, 0, 1}, targets, 6, &collision));
}
} // namespace
