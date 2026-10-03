#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PartyCombo.h"
#include "game/screens/PlayerHealth.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

/** A flat floor with a wall across it at z = 20, facing back the way the party comes. */
WorldCollision arena() {
    std::vector<CollisionTriangle> triangles;
    const auto add = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
        CollisionTriangle triangle;
        triangle.vertices = {a, b, c};
        triangle.normal = normal;
        triangles.push_back(triangle);
    };
    add(Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}, Vec3{0, 1, 0});
    add(Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}, Vec3{0, 1, 0});
    add(Vec3{-100, 0, 20}, Vec3{100, 0, 20}, Vec3{100, 20, 20}, Vec3{0, 0, -1});
    add(Vec3{-100, 0, 20}, Vec3{100, 20, 20}, Vec3{-100, 20, 20}, Vec3{0, 0, -1});
    WorldCollision collision;
    collision.build(triangles);
    return collision;
}

struct Fixture {
    std::array<PlayerRuntime, 2> players;
    WorldCollision collision = arena();
    std::vector<std::string> calls;
    bool struck = false;
    PartyCombo::Events events{
        .impact =
            [this](usize flier, usize thrower, f32 blow) {
                calls.push_back("impact" + std::to_string(flier) + "by" + std::to_string(thrower) +
                                "for" + std::to_string(static_cast<s32>(blow)));
                return struck;
            },
        .advanceTurbo = [this](usize i, s32,
                               f32) { calls.push_back("turbo" + std::to_string(i)); }};
    Fixture() {
        players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 3}, 0);
        for (PlayerRuntime& player : players) {
            player.floor.object = 0;
        }
    }
};

TEST_CASE("invalid combo pairs release both participants and restore attached positions",
          "[game][screens][combo][multiplayer]") {
    Fixture f;
    PartyCombo::begin(f.players, 0, 1);
    f.players[1].combo.riding = true;
    f.players[1].combo.saved = {2, 0, 3};
    f.players[1].actor.place({0, 10, 0});
    SECTION("initiator died") {
        f.players[0].life = PlayerLife::Dying;
    }
    SECTION("partner died") {
        f.players[1].life = PlayerLife::Dying;
    }
    SECTION("initiator left") {
        f.players[0].departed = true;
    }
    SECTION("partner left") {
        f.players[1].departed = true;
    }
    SECTION("initiator no longer owns the pairing") {
        ComboMove::clear(f.players[0].combo);
    }
    PartyCombo::cancelInvalid(f.players);
    CHECK_FALSE(f.players[0].combo.active());
    CHECK_FALSE(f.players[1].combo.active());
    CHECK(f.players[1].actor.position() == Vec3{2, 0, 3});
    PartyCombo::cancelInvalid(f.players);
    CHECK(f.players[1].actor.position() == Vec3{2, 0, 3});
}

TEST_CASE("combo cleanup preserves an unrelated pair when a slot is reused",
          "[game][screens][combo][multiplayer]") {
    std::array<PlayerRuntime, 4> players;
    PartyCombo::begin(players, 0, 1);
    PartyCombo::begin(players, 1, 3);
    PartyCombo::cancelInvalid(players);
    CHECK_FALSE(players[0].combo.active());
    CHECK(players[1].combo.role == ComboRole::Grabber);
    CHECK(players[3].combo.role == ComboRole::Held);
}

TEST_CASE("nobody without a figure takes hold or is taken hold of", "[game][screens][combo]") {
    Fixture f;
    REQUIRE(PartyCombo::partnerFor(f.players, 0) == std::nullopt);
    REQUIRE_FALSE(PartyCombo::aside(f.players[0]));
    REQUIRE(PartyCombo::deedOf(f.players[0]) == PlayerDeed::None);
    // Tied by hand, the roles read as the combo's deeds.
    PartyCombo::begin(f.players, 0, 1);
    REQUIRE(f.players[0].combo.role == ComboRole::Grabber);
    REQUIRE(f.players[1].combo.role == ComboRole::Held);
    REQUIRE(PartyCombo::deedOf(f.players[1]) == PlayerDeed::ComboHeld);
    REQUIRE(PartyCombo::aside(f.players[1]));
    REQUIRE_FALSE(PartyCombo::aside(f.players[0]));
    // Without a figure the grabber's phase is nothing: a warrior's partner is let fly.
    PartyCombo::advance(f.players, 0, kTicks);
    REQUIRE(f.players[1].combo.role == ComboRole::Thrown);
    REQUIRE(PartyCombo::deedOf(f.players[1]) == PlayerDeed::ComboThrown);
    // A seat without a figure is the carrier itself.
    const Mat4 seat = PartyCombo::seatOf(f.players[0]);
    REQUIRE(Vec3{seat[3]} == f.players[0].actor.position());
}

TEST_CASE("every class pairing uses the initiator family and retains party-index ownership",
          "[game][screens][combo][multiplayer]") {
    for (s32 initiator = 0; initiator < kClassCount; ++initiator) {
        for (s32 partner = 0; partner < kClassCount; ++partner) {
            CAPTURE(initiator, partner);
            Fixture f;
            CharacterSave first;
            first.character = initiator;
            CharacterSave second;
            second.character = partner;
            f.players[0].actor.spawn(3, first, nullptr, Vec3{0, 0, 0}, 0);
            f.players[1].actor.spawn(1, second, nullptr, Vec3{0, 0, 3}, 0);
            PartyCombo::begin(f.players, 0, 1);
            const s32 family =
                initiator == kSumnerClass ? ComboMove::kWizard : initiator % kStartingClassCount;
            REQUIRE(f.players[0].combo.grabberClass == family);
            REQUIRE(f.players[1].combo.grabberClass == family);
            CHECK(f.players[0].combo.partner == 1);
            CHECK(f.players[1].combo.partner == 0);
            CHECK_FALSE(PlayerHealth::canBeDamaged(f.players[0]));
            CHECK_FALSE(PlayerHealth::canBeDamaged(f.players[1]));
            const ComboOrders attached = ComboMove::advance(f.players[0].combo, f.players[1].combo,
                                                            {true, false, 0}, kTicks);
            const bool initiatorRides = family == ComboMove::kValkyrie ||
                                        family == ComboMove::kArcher || family == ComboMove::kDwarf;
            CHECK(attached.attach == (initiatorRides ? ComboOrders::Attach::GrabberOnPartner
                                                     : ComboOrders::Attach::PartnerOnGrabber));
            // The link is not a distance leash; even a separated pinball completes its clock.
            f.players[1].actor.place(Vec3{100, 0, 100});
            PartyCombo::advance(f.players, 0, kTicks);
            if (family == ComboMove::kWarrior || family == ComboMove::kDwarf) {
                CHECK(f.players[1].combo.role == ComboRole::Thrown);
                CHECK_FALSE(PlayerHealth::canBeDamaged(f.players[0]));
                CHECK_FALSE(PlayerHealth::canBeDamaged(f.players[1]));
                PartyCombo::advance(f.players, 0, ComboMove::kFlightTicks);
            }
            CHECK_FALSE(f.players[0].combo.active());
            CHECK_FALSE(f.players[1].combo.active());
            CHECK_FALSE(f.players[0].combo.riding);
            CHECK_FALSE(f.players[1].combo.riding);
            CHECK(PlayerHealth::canBeDamaged(f.players[0]));
            CHECK(PlayerHealth::canBeDamaged(f.players[1]));
        }
    }
}

TEST_CASE("a held partner spends thrower grace during the windup",
          "[game][screens][combo][multiplayer]") {
    Fixture f;
    PartyCombo::begin(f.players, 0, 1);
    ComboMove::advance(f.players[0].combo, f.players[1].combo, {true, false, 0}, kTicks);
    PartyCombo::animate(f.players, 1, 60, 1.0f, f.events);
    REQUIRE(f.players[1].combo.graceSeconds == Approx(0.5f));
    PartyCombo::advance(f.players, 0, kTicks);
    REQUIRE(ComboMove::flies(f.players[1].combo));
    f.players[1].actor.place(Vec3{0, 0, 0});
    f.players[0].actor.place(Vec3{0, 0, 2});
    PartyCombo::fly(f.players, 1, 30, 0.5f, f.collision, f.events);
    CHECK(f.players[1].combo.graceSeconds == 0.0f);
}

TEST_CASE("a pinball flies thirty a second along its facing and reflects off a wall",
          "[game][screens][combo]") {
    Fixture f;
    PartyCombo::begin(f.players, 0, 1);
    f.players[1].combo.role = ComboRole::Thrown;
    f.players[1].actor.place(Vec3{0, 0, 10});
    f.players[1].actor.turnTo(0.0f); // along +z, at the wall
    PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    REQUIRE(f.players[1].actor.position().z == Approx(10.0f + ComboMove::kFlightSpeed * kStep));
    REQUIRE(f.calls == std::vector<std::string>{"impact1by0for50"});
    // Ten frames on it meets the wall and comes back the way it went.
    for (s32 i = 0; i < 12; ++i) {
        PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    }
    REQUIRE(f.players[1].actor.position().z < 20.0f);
    REQUIRE(std::abs(f.players[1].actor.yaw()) == Approx(std::numbers::pi_v<f32>).margin(0.01f));
    // Striking an item turns it three eighths round, once in ten ticks.
    f.players[1].actor.turnTo(0.0f);
    f.players[1].actor.place(Vec3{5, 0, 0});
    f.players[1].combo.turnTicks = 0;
    f.struck = true;
    PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    REQUIRE(f.players[1].actor.yaw() == Approx(ComboMove::kBounceTurn));
    PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    REQUIRE(f.players[1].actor.yaw() == Approx(ComboMove::kBounceTurn)); // too soon to turn again
}

TEST_CASE("a pinball passes through its thrower for a while and then bounces off it",
          "[game][screens][combo]") {
    Fixture f;
    PartyCombo::begin(f.players, 0, 1);
    f.players[1].combo.role = ComboRole::Thrown;
    f.players[1].combo.graceSeconds = ComboMove::kThrowerGrace;
    f.players[0].actor.place(Vec3{0, 0, 2});
    f.players[1].actor.place(Vec3{0, 0, 0});
    f.players[1].actor.turnTo(0.0f);
    PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    REQUIRE(f.players[1].actor.yaw() == Approx(0.0f)); // through the thrower, unturned
    CHECK(f.players[1].actor.position().z == Approx(ComboMove::kFlightSpeed * kStep));
    f.players[0].knockback.endFrame();
    CHECK_FALSE(f.players[0].knockback.pushed());
    f.players[1].combo.graceSeconds = 0.0f;
    f.players[1].actor.place(Vec3{0, 0, 0});
    PartyCombo::fly(f.players, 1, kTicks, kStep, f.collision, f.events);
    REQUIRE(f.players[1].actor.yaw() == Approx(ComboMove::kBounceTurn));
    f.players[0].knockback.endFrame();
    CHECK(f.players[0].knockback.pushed());
}

TEST_CASE("a charger goes where the dwarf's stick sends it at half again the pace",
          "[game][screens][combo]") {
    Fixture f;
    PartyCombo::begin(f.players, 0, 1);
    f.players[0].combo.grabberClass = ComboMove::kDwarf;
    f.players[1].combo.grabberClass = ComboMove::kDwarf;
    f.players[1].combo.role = ComboRole::Thrown;
    f.players[1].actor.place(Vec3{0, 0, 10});
    f.players[1].actor.turnTo(0.0f);
    const f32 pace = f.players[1].actor.speed();
    PartyCombo::ride(f.players, 1, MoveInput{Vec2{0, 1}, 1}, 0.0f, kTicks, kStep, f.collision,
                     f.events);
    REQUIRE(f.players[1].actor.position().z == Approx(10.0f + pace * ComboMove::kRidePace * kStep));
    REQUIRE(f.calls == std::vector<std::string>{"impact1by0for10"});
    // The stick across turns it only half way in a frame.
    PartyCombo::ride(f.players, 1, MoveInput{Vec2{1, 0}, 1}, 0.0f, kTicks, kStep, f.collision,
                     f.events);
    REQUIRE(f.players[1].actor.yaw() > 0.0f);
    REQUIRE(f.players[1].actor.yaw() < std::numbers::pi_v<f32> / 2.0f);
}

TEST_CASE("with the real warrior a partner ahead is taken hold of and rides its DUMMY node",
          "[game][screens][combo][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Fixture f;
    for (PlayerRuntime& player : f.players) {
        player.figure = PlayerFigure::load(device, root, player.actor.save());
        REQUIRE(player.figure != nullptr);
        for (s32 i = 0; i < 120; ++i) {
            player.figure->animate(0, kTicks, kStep);
        }
        REQUIRE(player.figure->animator().action() == PlayerAnimator::Action::Ready);
    }
    // Facing away from the partner nobody is ahead; turned to it, it is.
    f.players[0].actor.turnTo(std::numbers::pi_v<f32>);
    REQUIRE(PartyCombo::partnerFor(f.players, 0) == std::nullopt);
    f.players[0].actor.turnTo(0.0f);
    REQUIRE(PartyCombo::partnerFor(f.players, 0) == 1);
    // fn_80088EF4 requires both players to stand on a nonmoving floor.
    for (PlayerRuntime& player : f.players) {
        player.floor.object = -1;
        CHECK_FALSE(PartyCombo::partnerFor(f.players, 0).has_value());
        player.floor.object = 0;
        player.floor.flags = FloorRiding::kMoving;
        CHECK_FALSE(PartyCombo::partnerFor(f.players, 0).has_value());
        player.floor.flags = 0;
        CHECK(PartyCombo::partnerFor(f.players, 0) == 1);
    }
    PartyCombo::begin(f.players, 0, 1);
    REQUIRE(f.players[1].combo.grabberClass == ComboMove::kWarrior);
    // The grabber's move begins; a tick on, the partner is in its hands and plays COMBOWAR1.
    f.players[0].figure->animate(0, kTicks, kStep, PlayerDeed::Combo);
    REQUIRE(f.players[0].figure->animator().action() == PlayerAnimator::Action::ComboAct1);
    PartyCombo::advance(f.players, 0, kTicks);
    REQUIRE(f.players[1].combo.riding);
    REQUIRE(f.players[1].combo.saved == Vec3{0, 0, 3});
    // The restriction is on taking hold, not on continuing the carried move.
    f.players[1].floor = {};
    PartyCombo::animate(f.players, 1, kTicks, kStep, f.events);
    REQUIRE(f.players[1].figure->animator().action() == PlayerAnimator::Action::ComboWar1);
    CHECK(f.players[1].combo.graceSeconds == Approx(ComboMove::kThrowerGrace - kStep));
    PartyCombo::advance(f.players, 0, kTicks);
    CHECK(f.players[1].combo.graceSeconds == Approx(ComboMove::kThrowerGrace - kStep));
    REQUIRE(f.calls == std::vector<std::string>{"turbo1"});
    f.players[0].actor.place(Vec3{5, 0, 5});
    PartyCombo::carry(f.players);
    const Mat4 seat = PartyCombo::seatOf(f.players[0]);
    REQUIRE(f.players[1].actor.position() == Vec3{seat[3]});
    // The node rides the root's own motion through COMBOACT1: near the warrior, ahead of it.
    REQUIRE(glm::distance(f.players[1].actor.position(), f.players[0].actor.position()) < 4.0f);
    REQUIRE(f.players[1].actor.position().x == Approx(5.0f).margin(0.5f));
    REQUIRE(f.players[1].actor.yaw() == Approx(0.0f).margin(0.01f));
}

} // namespace
