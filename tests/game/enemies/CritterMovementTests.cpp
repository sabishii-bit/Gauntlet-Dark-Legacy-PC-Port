#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/enemies/CritterData.h"
#include "game/enemies/CritterMovement.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
constexpr f32 kPi = std::numbers::pi_v<f32>;

TEST_CASE("boss territories constrain displacement not move speed", "[game][boss-movement]") {
    CritterMovement movement;
    const Vec3 home{10, 7, -20};
    movement.roamRadius = 5;
    const Vec3 limited = movement.constrain(home + Vec3{30, 2, 40}, home);
    REQUIRE(limited == Vec3{13, 9, -16});
    REQUIRE(movement.constrain(home + Vec3{1, 0, 1}, home) == home + Vec3{1, 0, 1});
    movement.squareBounds = true;
    REQUIRE(movement.constrain(home + Vec3{30, 2, -40}, home) == home + Vec3{5, 2, -5});
    movement.roamRadius = 0;
    REQUIRE(movement.constrain(home + Vec3{30, 0, 40}, home) == home);
}

TEST_CASE("boss turning respects the initial facing and wraparound", "[game][boss-movement]") {
    CritterMovement movement;
    movement.turnLimit = 0;
    REQUIRE(movement.facing(2, 0) == 0); // dragon cannot swivel toward the player
    movement.turnLimit = 0.25f;
    REQUIRE(movement.facing(2, 0) == Approx(0.25f));
    REQUIRE(movement.facing(-2, 0) == Approx(-0.25f));
    REQUIRE(movement.facing(-kPi + 0.05f, kPi - 0.05f) == Approx(-kPi + 0.05f));
    movement.unrestrictedTurn = true;
    REQUIRE(movement.facing(2, 0) == 2);
}

TEST_CASE("pursuing critters advance retreat and sidestep in distinct directions",
          "[game][boss-movement]") {
    REQUIRE(CritterMovement::direction(MoveDefinition::kWalk, 0) == Vec3{0, 0, 1});
    REQUIRE(CritterMovement::direction(MoveDefinition::kStepBack, 0) == Vec3{0, 0, -1});
    REQUIRE(CritterMovement::direction(MoveDefinition::kStepLeft, 0) == Vec3{-1, 0, 0});
    REQUIRE(CritterMovement::direction(MoveDefinition::kStepRight, 0) == Vec3{1, 0, 0});
    REQUIRE(CritterMovement::direction(134, kPi / 2).x == Approx(1)); // authored charge
    REQUIRE(CritterMovement::direction(MoveDefinition::kStepToPoint, 0) == Vec3{0});
}

TEST_CASE("movement manifests read the home sentinel limits and legacy radius key",
          "[game][boss-movement]") {
    const auto file = test::scratchDirectory("critter-movement") / "critter.json";
    writeTextFile(file, R"({"descriptors":[{"prefix":"BOSS","type":4}],
      "types":[{"speed":22,"defaultPos":[-30,57,-112],"turnLimit":0.5,"typeFlags":1120,"moveCount":1}],
      "moves":[{"name":"READY","type":32,"anim":"READY"}]})");
    CritterData data;
    REQUIRE(data.load(file));
    REQUIRE(data.movement().roamRadius == 22);
    REQUIRE(data.movement().home == Vec3{-30, 57, -112});
    REQUIRE(data.movement().turnLimit == 0.5f);
    REQUIRE(data.movement().squareBounds);
    REQUIRE(data.movement().initialStepBasis);
    REQUIRE(data.movement().unrestrictedTurn);
    writeTextFile(file, R"({"descriptors":[{"prefix":"BOSS","type":4}],
      "types":[{"roamRadius":0,"speed":22,"defaultPos":[0,999,0],"turnLimit":0,"moveCount":1}],
      "moves":[{"name":"READY","type":32,"anim":"READY"}]})");
    REQUIRE(data.load(file));
    REQUIRE(data.movement().roamRadius == 0); // corrected key wins over the old spelling
    REQUIRE_FALSE(data.movement().home.has_value());
    REQUIRE_FALSE(data.movement().initialStepBasis);
    REQUIRE_FALSE(data.movement().squareBounds);
    REQUIRE_FALSE(data.movement().unrestrictedTurn);
}

} // namespace
