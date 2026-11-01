#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/critters/CombatantBreath.h"
#include "game/critters/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("breath transforms the offset but normalizes the animated forward basis",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.maxDistance = 30;
    damage.offset = {0, -4, -3};
    const Mat4 node = glm::scale(glm::translate(Mat4{1}, Vec3{10, 20, 30}), Vec3{2});
    const auto breath = CombatantBreath::fromNode(node, damage);
    REQUIRE(breath.origin == Vec3{10, 12, 24});
    REQUIRE(breath.end == Vec3{10, 12, 54});

    const Mat4 turned = glm::rotate(node, std::numbers::pi_v<f32> / 2, Vec3{0, 1, 0});
    const auto moved = CombatantBreath::fromNode(turned, damage);
    REQUIRE(moved.origin.x == Approx(4));
    REQUIRE(moved.origin.z == Approx(30));
    REQUIRE(moved.end.x == Approx(34));
    REQUIRE(moved.end.z == Approx(30));
}

TEST_CASE("breath applies authored yaw then pitch, including an inclined node",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.maxDistance = 10;
    damage.yaw = std::numbers::pi_v<f32> / 2;
    damage.pitch = std::numbers::pi_v<f32> / 6;
    const auto breath = CombatantBreath::fromNode(Mat4{1}, damage);
    REQUIRE(breath.end.x == Approx(-8.660254f));
    REQUIRE(breath.end.y == Approx(-5));
    REQUIRE(breath.end.z == Approx(0).margin(0.00001));

    Mat4 inclined{1};
    inclined[2] = Vec4{0, 0.6f, 0.8f, 0};
    damage.yaw = 0;
    const auto tipped = CombatantBreath::fromNode(inclined, damage);
    REQUIRE(tipped.end.x == 0);
    REQUIRE(tipped.end.y == Approx(10 * (0.6f * std::cos(damage.pitch) - 0.5f * 0.64f)));
    REQUIRE(tipped.end.z == Approx(10 * (0.5f * 0.48f + 0.8f * std::cos(damage.pitch))));
}

TEST_CASE("breath uses a cylinder and an unexpanded horizontal distance window",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.radius = 0.5f;
    damage.minDistance = 2;
    damage.maxDistance = 10;
    damage.minDot = 1; // Not a cone test: this value must not restrict collision.
    const CombatantBreath breath{{0, 3, 0}, {0, 3, 10}};
    REQUIRE(breath.touches(damage, {0, 3, 2}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 1.99f}, 1, 3));
    REQUIRE(breath.touches(damage, {0, 3, 10}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 10.01f}, 1, 3));
    REQUIRE(breath.touches(damage, {1.5f, 6.5f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {1.501f, 3, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 6.501f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, -5}, 1, 3));
    // The closest point is three-dimensional, not a projection onto the ground.
    const CombatantBreath down{{0, 10, 0}, {0, 0, 10}};
    REQUIRE(down.touches(damage, {0, 5, 5}, 0.1f, 0.1f));
    REQUIRE_FALSE(down.touches(damage, {0, 0, 5}, 0.1f, 0.1f));
    damage.minDistance = 0;
    const CombatantBreath point{{0, 3, 0}, {0, 3, 0}};
    REQUIRE(point.touches(damage, {0, 3, 0}, 1, 3));
    REQUIRE_FALSE(point.touches(damage, {0, 3, 5}, 1, 3));
}

TEST_CASE("imported world-axis breath pitch acts after the posed forward and authored yaw",
          "[game][enemies][breath][native-breath]") {
    AttackDefinition damage;
    damage.maxDistance = 10;
    damage.yaw = std::numbers::pi_v<f32> / 2;
    damage.pitch = std::numbers::pi_v<f32> / 6;
    // Native yaw turns +Z into -X. World-X pitch cannot then tip it downward,
    // unlike GDL's direction-dependent horizontal perpendicular.
    const auto native = CombatantBreath::fromNode(Mat4{1}, damage, true);
    CHECK(native.end.x == Approx(-10));
    CHECK(native.end.y == Approx(0).margin(0.00001));
    CHECK(native.end.z == Approx(0).margin(0.00001));
    const auto ordinary = CombatantBreath::fromNode(Mat4{1}, damage);
    CHECK(ordinary.end.x == Approx(-8.660254f));
    CHECK(ordinary.end.y == Approx(-5));
    CHECK(ordinary.origin == native.origin);

    Mat4 posed = glm::translate(Mat4{1}, Vec3{10, 20, 30});
    posed[2] = Vec4{1.2f, 1.6f, 0, 0};
    damage.yaw = 0;
    damage.offset = {0, 0, 2};
    const auto inclined = CombatantBreath::fromNode(posed, damage, true);
    CHECK(inclined.origin.x == Approx(12.4f));
    CHECK(inclined.origin.y == Approx(23.2f));
    CHECK(inclined.origin.z == Approx(30));
    const Vec3 direction = (inclined.end - inclined.origin) / damage.maxDistance;
    CHECK(direction.x == Approx(0.6f));
    CHECK(direction.y == Approx(0.8f * std::cos(damage.pitch)));
    CHECK(direction.z == Approx(0.4f));
    CHECK(glm::length(inclined.end - inclined.origin) == Approx(damage.maxDistance));

    posed[2] = Vec4{0};
    const auto zero = CombatantBreath::fromNode(posed, damage, true);
    CHECK(zero.end == zero.origin);
}

TEST_CASE("critter damage loading preserves the breath's inner distance limit",
          "[game][enemies][breath]") {
    const auto file = test::scratchDirectory("breath-damage") / "creature.json";
    writeTextFile(file,
                  R"({"name":"TEST", "types":[{"moveCount":1}], "descriptors":[{}], "moves":[{}],
                         "damages":[{"type":4,"minDistance":7.5,"maxDistance":30}]})");
    CritterData data;
    REQUIRE(data.load(file));
    REQUIRE(data.damages().size() == 1);
    REQUIRE(data.damages()[0].minDistance == 7.5f);
}
} // namespace
