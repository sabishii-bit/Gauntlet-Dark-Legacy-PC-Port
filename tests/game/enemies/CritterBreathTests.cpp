#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/enemies/CritterBreath.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("breath transforms the offset but normalizes the animated forward basis",
          "[game][enemies][breath]") {
    CritterDamage damage;
    damage.maxDistance = 30;
    damage.offset = {0, -4, -3};
    const Mat4 node = glm::scale(glm::translate(Mat4{1}, Vec3{10, 20, 30}), Vec3{2});
    const auto breath = CritterBreath::fromNode(node, damage);
    REQUIRE(breath.origin == Vec3{10, 12, 24});
    REQUIRE(breath.end == Vec3{10, 12, 54});

    const Mat4 turned = glm::rotate(node, std::numbers::pi_v<float> / 2, Vec3{0, 1, 0});
    const auto moved = CritterBreath::fromNode(turned, damage);
    REQUIRE(moved.origin.x == Approx(4));
    REQUIRE(moved.origin.z == Approx(30));
    REQUIRE(moved.end.x == Approx(34));
    REQUIRE(moved.end.z == Approx(30));
}

TEST_CASE("breath applies authored yaw then pitch, including an inclined node",
          "[game][enemies][breath]") {
    CritterDamage damage;
    damage.maxDistance = 10;
    damage.yaw = std::numbers::pi_v<float> / 2;
    damage.pitch = std::numbers::pi_v<float> / 6;
    const auto breath = CritterBreath::fromNode(Mat4{1}, damage);
    REQUIRE(breath.end.x == Approx(-8.660254f));
    REQUIRE(breath.end.y == Approx(-5));
    REQUIRE(breath.end.z == Approx(0).margin(0.00001));

    Mat4 inclined{1};
    inclined[2] = Vec4{0, 0.6f, 0.8f, 0};
    damage.yaw = 0;
    const auto tipped = CritterBreath::fromNode(inclined, damage);
    REQUIRE(tipped.end.x == 0);
    REQUIRE(tipped.end.y == Approx(10 * (0.6f * std::cos(damage.pitch) - 0.5f * 0.64f)));
    REQUIRE(tipped.end.z == Approx(10 * (0.5f * 0.48f + 0.8f * std::cos(damage.pitch))));
}

TEST_CASE("breath uses a cylinder and an unexpanded horizontal distance window",
          "[game][enemies][breath]") {
    CritterDamage damage;
    damage.radius = 0.5f;
    damage.minDistance = 2;
    damage.maxDistance = 10;
    damage.minDot = 1; // Not a cone test: this value must not restrict collision.
    const CritterBreath breath{{0, 3, 0}, {0, 3, 10}};
    REQUIRE(breath.touches(damage, {0, 3, 2}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 1.99f}, 1, 3));
    REQUIRE(breath.touches(damage, {0, 3, 10}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 10.01f}, 1, 3));
    REQUIRE(breath.touches(damage, {1.5f, 6.5f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {1.501f, 3, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 6.501f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, -5}, 1, 3));
    // The closest point is three-dimensional, not a projection onto the ground.
    const CritterBreath down{{0, 10, 0}, {0, 0, 10}};
    REQUIRE(down.touches(damage, {0, 5, 5}, 0.1f, 0.1f));
    REQUIRE_FALSE(down.touches(damage, {0, 0, 5}, 0.1f, 0.1f));
    damage.minDistance = 0;
    const CritterBreath point{{0, 3, 0}, {0, 3, 0}};
    REQUIRE(point.touches(damage, {0, 3, 0}, 1, 3));
    REQUIRE_FALSE(point.touches(damage, {0, 3, 5}, 1, 3));
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
