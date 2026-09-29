#include <array>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/screens/PartyCollision.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::array<PlayerRuntime, 3> party() {
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 10}, 0);
    players[2].actor.spawn(2, {}, nullptr, Vec3{0, 0, -10}, 0);
    return players;
}

TEST_CASE("a step into another member ends pushed out to their radii's sum",
          "[game][screens][party-collision]") {
    auto players = party();
    const f32 reach = players[0].actor.radius() + players[1].actor.radius();
    // Straight at them, it stops short of them.
    Vec3 to{0, 0, 9.5f};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0, 0, 7}, to) == std::optional<usize>{1});
    CHECK(to.z == Approx(10.0f - reach));
    CHECK(to.x == Approx(0.0f));
    // Glancing, it is pushed aside around them.
    to = Vec3{0.5f, 0, 9.0f};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0.5f, 0, 7}, to).has_value());
    CHECK(glm::distance(Vec2{to.x, to.z}, Vec2{0, 10}) == Approx(reach));
    CHECK(to.x > 0.5f);
    // What is out of reach of the step, or behind it, or not standing, is let be.
    to = Vec3{0, 0, 2};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, 0}, to).has_value());
    const f32 near = -10.0f + reach * 0.5f;
    to = Vec3{0, 0, near + 0.1f};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to).has_value());
    to = Vec3{0, 0, near - 0.1f};
    CHECK(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to) == std::optional<usize>{2});
    players[2].life = PlayerLife::InTower;
    to = Vec3{0, 0, near - 0.1f};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to).has_value());
    // A step ending on top of another is undone.
    to = Vec3{0, 0, 10};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0, 0, 8}, to).has_value());
    CHECK(to == Vec3{0, 0, 8});
}

TEST_CASE("the one run into is shoved by the step as it was meant",
          "[game][screens][party-collision]") {
    auto players = party();
    players[0].actor.place(Vec3{0, 0, 9.5f});
    PartyCollision::step(players, 0, Vec3{0, 0, 9}, 1.0f / 30.0f);
    CHECK(players[0].actor.position().z < 9.0f);
    players[1].knockback.endFrame();
    CHECK(players[1].knockback.pushed());
    CHECK_FALSE(players[2].knockback.pushed());
    // The shove carries it on a little, the way it was pushed.
    const Vec3 moved = players[1].knockback.step(1.0f / 30.0f, 10.0f);
    CHECK(moved.z > 0.0f);
    CHECK(moved.x == 0.0f);
}

} // namespace
