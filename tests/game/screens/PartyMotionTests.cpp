#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/screens/PartyMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    std::array<PlayerRuntime, 2> players;
    std::array<PlayInput, 4> inputs;
    WorldCollision collision;
    std::vector<std::string> calls;
    PartyMotion::Events events{
        .perform =
            [this](std::size_t i, PartyMotion::Action action) {
                REQUIRE(action == PartyMotion::Action::NoPotion);
                calls.push_back("help" + std::to_string(i));
            },
        .select =
            [this](std::size_t i, const SelectorInput&, std::int32_t ticks) {
                REQUIRE(ticks == 2);
                calls.push_back("select" + std::to_string(i));
            },
        .advanceTurbo = [](std::size_t, std::int32_t,
                           float) { FAIL("No figure, no animation events"); }};

    Fixture() {
        CollisionTriangle first;
        first.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}};
        first.normal = Vec3{0, 1, 0};
        CollisionTriangle second;
        second.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}};
        second.normal = first.normal;
        collision.build({first, second});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{10, 0, 0}, 0);
    }
    std::vector<CameraSubject> step(bool held = false) {
        return PartyMotion::step(players, inputs, held, 0, 2, 1.0f / 30.0f, collision, events);
    }
};

TEST_CASE("party motion routes sparse input ids and snapshots after movement",
          "[game][screens][party-motion]") {
    Fixture f;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].usePotion = true;
    f.inputs[3].shieldPotion = true;
    f.inputs[1].strongAttack = true; // Missing artwork must not dereference a null figure.
    f.players[0].rammed.push_back(42);
    const auto subjects = f.step();
    REQUIRE(f.calls == std::vector<std::string>{"help0", "help0", "select0", "select1"});
    REQUIRE(f.players[0].actor.position().z > 0);
    REQUIRE(f.players[1].actor.position().z == 0);
    REQUIRE(subjects.size() == 2);
    REQUIRE(subjects[0].feet == f.players[0].actor.position());
    REQUIRE(subjects[0].follow == f.players[0].actor.followPoint());
    REQUIRE(f.players[0].rammed.empty());
    f.players[0].actor.place(Vec3{99, 0, 99});
    REQUIRE(subjects[0].feet.z != 99);
}

TEST_CASE("party motion holds input and consumes reactions without advancing missing figures",
          "[game][screens][party-motion]") {
    Fixture f;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].usePotion = true;
    f.players[0].reaction = PlayerDeed::Flinch;
    f.players[1].life = PlayerLife::Dying;
    f.step();
    REQUIRE(f.calls.empty());
    REQUIRE(f.players[0].actor.position().z == 0);
    REQUIRE(f.players[0].reaction == PlayerDeed::None);
    REQUIRE(f.players[1].life == PlayerLife::InTower);
    f.step(true);
    REQUIRE(f.calls.empty());
    REQUIRE(f.players[0].actor.position().z == 0);
    f.step();
    REQUIRE(f.players[0].actor.position().z > 0);
    REQUIRE(f.calls == std::vector<std::string>{"help0", "select0"});
}

TEST_CASE("party motion ignores invalid player ids", "[game][screens][party-motion]") {
    Fixture f;
    f.players[0].actor.spawn(-1, {}, nullptr, Vec3{0}, 0);
    f.players[1].actor.spawn(4, {}, nullptr, Vec3{0}, 0);
    f.step();
    REQUIRE(f.calls.empty());
}

TEST_CASE("charge steering and strafe directions remain camera relative",
          "[game][screens][party-motion]") {
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, Vec3{0}, std::numbers::pi_v<float> / 2);
    const auto straight = PartyMotion::chargeInput(actor, {}, 0);
    REQUIRE(straight.magnitude == 1);
    REQUIRE(straight.direction.x == Approx(1));
    const MoveInput pushed{Vec2{-1, 0}, 0.25f};
    REQUIRE(PartyMotion::chargeInput(actor, pushed, 0).direction == pushed.direction);
    REQUIRE(PartyMotion::strafeWayOf(0, 0) == StrafeWay::Forward);
    REQUIRE(PartyMotion::strafeWayOf(std::numbers::pi_v<float>, 0) == StrafeWay::Back);
    REQUIRE(PartyMotion::strafeWayOf(1.0f, 0) == StrafeWay::Right);
    REQUIRE(PartyMotion::strafeWayOf(-1.0f, 0) == StrafeWay::Left);
}
} // namespace
