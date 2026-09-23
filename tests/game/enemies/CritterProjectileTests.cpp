#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/enemies/CritterProjectile.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("critter projectiles interpolate the retail speed range without extrapolating",
          "[game][boss-projectiles]") {
    CritterDamage damage;
    damage.speed = 40;
    damage.maxSpeed = 80;
    REQUIRE(CritterProjectile::speed(damage, -1) == 40);
    REQUIRE(CritterProjectile::speed(damage, 0.5f) == 40);
    REQUIRE(CritterProjectile::speed(damage, 1) == 55);
    REQUIRE(CritterProjectile::speed(damage, 1.5f) == 70);
    REQUIRE(CritterProjectile::speed(damage, 20) == 70);
}

TEST_CASE("critter ballistic projectiles reach the target at fixed horizontal speed",
          "[game][boss-projectiles]") {
    CritterDamage damage;
    damage.speed = damage.maxSpeed = 20;
    damage.gravity = 10;
    damage.behaviorFlags = CritterProjectile::kAimAtPlayer;
    CritterShot shot;
    shot.origin = {10, 7, 10};
    shot.target = Vec3{40, 3, 50};
    const Vec3 velocity = CritterProjectile::velocity(damage, shot);
    const float flight = 2.5f;
    const Vec3 end =
        shot.origin + velocity * flight - Vec3{0, 0.5f * damage.gravity * flight * flight, 0};
    REQUIRE(glm::length(end - *shot.target) < 0.001f);
    REQUIRE(glm::length(Vec2{velocity.x, velocity.z}) == Approx(20));
    damage.pitch = 1; // ballistic aiming does not apply the straight-shot pitch
    REQUIRE(CritterProjectile::velocity(damage, shot) == velocity);
    damage.speed = 0;
    REQUIRE(CritterProjectile::velocity(damage, shot) == Vec3{0});
}

TEST_CASE("critter straight shots honor facing precedence yaw pitch and spread",
          "[game][boss-projectiles]") {
    CritterDamage damage;
    damage.speed = damage.maxSpeed = 10;
    damage.behaviorFlags = CritterProjectile::kStraight | CritterProjectile::kAimAtPlayer;
    CritterShot shot;
    shot.target = Vec3{10, 0, 0};
    REQUIRE(CritterProjectile::velocity(damage, shot) == Vec3{10, 0, 0});
    damage.behaviorFlags |= CritterProjectile::kBodyForward;
    REQUIRE(CritterProjectile::velocity(damage, shot) == Vec3{0, 0, 10});
    damage.yawSpread = std::numbers::pi_v<float>;
    REQUIRE(CritterProjectile::velocity(damage, shot, 1).x == Approx(-10));
    REQUIRE(CritterProjectile::velocity(damage, shot, -1).x == Approx(10));
    damage.yawSpread = 0;
    damage.pitch = -std::numbers::pi_v<float> / 2;
    REQUIRE(CritterProjectile::velocity(damage, shot).y == Approx(10));
    shot.forward = Vec3{0};
    REQUIRE(glm::length(CritterProjectile::velocity(damage, shot)) == 0);
}

TEST_CASE("critter projectile sweeps intersect the cylinder at first entry not closest centre",
          "[game][boss-projectiles]") {
    REQUIRE(CritterProjectile::contact({0, 3, -10}, {0, 3, 10}, 1, Vec3{0}, 1, 6) == Approx(0.4f));
    REQUIRE_FALSE(CritterProjectile::contact({5, 3, -10}, {5, 3, 10}, 1, Vec3{0}, 1, 6));
    REQUIRE_FALSE(CritterProjectile::contact({0, 9, -10}, {0, 9, 10}, 1, Vec3{0}, 1, 6));
    REQUIRE(CritterProjectile::contact({0, 10, 0}, {0, 0, 0}, 1, Vec3{0}, 1, 6) == Approx(0.3f));
    REQUIRE(CritterProjectile::contact({0, 3, 0}, {0, 3, 0}, 1, Vec3{0}, 1, 6) == 0);
    REQUIRE_FALSE(CritterProjectile::contact({0, 3, 10}, {0, 3, 20}, 1, Vec3{0}, 1, 6));
}

TEST_CASE("critter projectile windows survive skipped frames and do not repeat held frames",
          "[game][boss-projectiles]") {
    CritterMove move;
    move.type = 132;
    move.frameStart = 14;
    REQUIRE(move.projectileTriggers(12, 16) == 1);
    REQUIRE(move.projectileTriggers(16, 16) == 0);
    REQUIRE(move.projectileTriggers(16, 20) == 0);
    REQUIRE(move.projectileTriggers(-1, 3, true) == 0);
    move.type = 133;
    move.frameStart = 12;
    move.frameEnd = 29;
    move.framePeriod = 4;
    REQUIRE(move.projectileTriggers(-1, 30) == 5);
    REQUIRE(move.projectileTriggers(12, 20) == 2);
    REQUIRE(move.projectileTriggers(20, 20) == 0);
    REQUIRE(move.projectileTriggers(29, 40) == 0);
    move.frameStart2 = 0;
    move.frameEnd2 = 4;
    REQUIRE(move.projectileTriggers(-1, 4, true) == 2);
}
} // namespace
