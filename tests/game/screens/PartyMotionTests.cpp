#include <algorithm>
#include <array>
#include <numbers>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelOpponents.h"
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
            [this](usize i, PartyMotion::Action action) {
                REQUIRE(action == PartyMotion::Action::NoPotion);
                calls.push_back("help" + std::to_string(i));
            },
        .select =
            [this](usize i, const SelectorInput&, s32 ticks) {
                REQUIRE(ticks == 2);
                calls.push_back("select" + std::to_string(i));
            },
        .advanceTurbo = [](usize, s32, f32) { FAIL("No figure, no animation events"); },
        .thrownImpact = {},
        .aim = {},
        .allowMovement = {},
        .attackDeed = {},
        .resolveMovement = {}};

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

TEST_CASE("a damage flash expires after two simulation frames without holding controls",
          "[game][screens][party-motion]") {
    Fixture f;
    f.players[0].hitFlashTicks = 4;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.step();
    CHECK(f.players[0].hitFlashTicks == 2);
    CHECK(f.players[0].actor.position().z > 0);
    f.step();
    CHECK(f.players[0].hitFlashTicks == 0);
    f.step();
    CHECK(f.players[0].hitFlashTicks == 0);
}

TEST_CASE("transport holds only its own player and clears on death",
          "[party-motion][transporters]") {
    Fixture f;
    REQUIRE(f.players[0].transport.begin(Vec3{20, 0, 0}));
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].usePotion = true;
    f.inputs[1].move = MoveInput{Vec2{0, 1}, 1};
    f.step();
    CHECK(f.players[0].actor.position() == Vec3{0});
    CHECK_FALSE(f.players[0].actor.moving());
    CHECK(f.players[1].actor.position().z > 0);
    CHECK(std::ranges::find(f.calls, "help0") == f.calls.end());
    f.players[0].life = PlayerLife::Dying;
    f.step();
    CHECK_FALSE(f.players[0].transport.active());
    CHECK(f.players[0].transport.alpha() == 1.0f);
}

TEST_CASE("stationary attacks face assisted targets without overriding movement or strafe",
          "[game][screens][party-motion][target-assist]") {
    Fixture f;
    f.inputs[3].attack = true;
    f.events.aim = [](usize) { return std::optional<Vec3>{{5, 3, 10}}; };
    f.step();
    REQUIRE(f.players[0].actor.yaw() == Approx(std::atan2(5.0f, 10.0f)));
    REQUIRE(f.players[0].actor.position() == Vec3{0});
    f.inputs[3].move = MoveInput{Vec2{-1, 0}, 1};
    f.step();
    REQUIRE(f.players[0].actor.yaw() == Approx(-std::numbers::pi_v<f32> / 2));
    f.inputs[3].move = {};
    f.inputs[3].strafe = true;
    f.step();
    REQUIRE(f.players[0].actor.yaw() == Approx(-std::numbers::pi_v<f32> / 2));
}

TEST_CASE("party motion consults the shared-view limit before reporting moved subjects",
          "[game][party-motion][camera-limit]") {
    Fixture f;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.events.allowMovement = [](const Vec3& before, const Vec3& after) {
        return after.z <= before.z;
    };
    const auto before = f.players[0].actor.position();
    const auto subjects = f.step();
    REQUIRE(f.players[0].actor.position().z == before.z);
    REQUIRE(subjects[0].feet == f.players[0].actor.position());
    f.inputs[3].move.direction.y = -1;
    f.step();
    REQUIRE(f.players[0].actor.position().z < before.z);
}

TEST_CASE("party motion resolves creatures before camera limits and snapshots",
          "[game][party-motion][collision]") {
    Fixture f;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.events.resolveMovement = [](usize, const Vec3& from, const Vec3& to) {
        return Vec3{from.x, to.y, from.z};
    };
    f.events.allowMovement = [](const Vec3& from, const Vec3& to) {
        CHECK(from == to);
        return true;
    };
    const auto subjects = f.step();
    CHECK(f.players[0].actor.position() == Vec3{0});
    CHECK(subjects[0].feet == Vec3{0});
}

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

TEST_CASE("pending web reactions slow movement without swallowing the escape input",
          "[game][party-motion][player-impact][spider]") {
    Fixture f;
    f.players[1].life = PlayerLife::InTower;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].attack = true;
    f.inputs[3].usePotion = true;
    f.step();
    const f32 normalStep = f.players[0].actor.position().z;
    REQUIRE(normalStep > 0);
    f.players[0].actor.place(Vec3{0});
    f.calls.clear();
    for (s32 frame = 0; frame < 120; ++frame) {
        f.players[0].reaction = PlayerDeed::Webbed;
        f.step();
        REQUIRE(f.players[0].actor.position().z == Approx(normalStep * 0.4f * (frame + 1)));
        REQUIRE(f.calls.empty());
    }
    const Vec3 before = f.players[0].actor.position();
    f.players[0].reaction = PlayerDeed::Webbed;
    f.step(true);
    REQUIRE(f.players[0].actor.position() == before);
    f.players[0].reaction = PlayerDeed::FallBack;
    f.step();
    REQUIRE(f.players[0].actor.position() == before);
}

TEST_CASE("arrival locks movement turning and buttons until the player animation ends",
          "[game][screens][party-motion][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/WAR/RED/objects.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Fixture f;
    f.events.advanceTurbo = [](usize, s32, f32) {};
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(device, root, player.actor.save());
    REQUIRE(player.figure != nullptr);
    f.inputs[3].move = MoveInput{Vec2{1, 0}, 1};
    f.inputs[3].usePotion = true;
    f.inputs[3].attack = true;
    const Vec3 start = player.actor.position();
    const f32 yaw = player.actor.yaw();
    s32 frames = 0;
    while (player.figure->animator().entering() && frames < 300) {
        f.calls.clear();
        f.step();
        REQUIRE(player.actor.position() == start);
        REQUIRE(player.actor.yaw() == yaw);
        // The other party member may receive input; the arriving member may not.
        REQUIRE(std::ranges::none_of(
            f.calls, [](const std::string& call) { return call == "help0" || call == "select0"; }));
        ++frames;
    }
    REQUIRE(frames > 10);
    REQUIRE(frames < 300);
    f.step();
    REQUIRE(player.actor.position() != start);
}

TEST_CASE("boss impacts reach retail player animations and lock input through recovery",
          "[game][screens][party-motion][player-impact][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/WAR/RED/objects.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
    test::FakeRenderDevice device;
    Fixture f;
    PlayerRuntime& player = f.players[0];
    player.figure = PlayerFigure::load(device, root, player.actor.save());
    REQUIRE(player.figure != nullptr);
    REQUIRE(player.figure->animator().bound());
    // Complete the entrance before combat, just as the scene does.
    for (s32 i = 0; i < 120; ++i) {
        player.figure->animate(0, 2, 1.0f / 30);
    }
    REQUIRE(player.figure->animator().action() == PlayerAnimator::Action::Ready);
    player.actor.save().progress().health = 1000;
    PlayerHealth health;
    const PlayerHealth::Events healthEvents{.block = [](f32, f32) {},
                                            .sound = [](std::string_view) {},
                                            .cry = [](std::string_view) {},
                                            .named = [](std::string_view) {}};
    LevelOpponents::Events opponents;
    opponents.hurt = [&](usize index, f32 damage, HurtKind kind, bool directed,
                         const PlayerImpact& impact) {
        REQUIRE(index == 0); // input id 3 is not party index 3
        health.hurt(f.players[index], damage, kind, directed, false, 1, healthEvents, impact);
    };
    CombatBlow blow;
    blow.player = 3;
    blow.damage = 10;
    blow.flags = PlayerImpact::kKnockDown;
    blow.direction = {0, 0, -1};
    using Action = PlayerAnimator::Action;
    PlayerDeed expected = PlayerDeed::FallBack;
    Action first = Action::FallBack;
    Action recovery = Action::GetUpBack;
    s32 remainingHealth = 990;
    SECTION("a frontal heavy hit knocks the player onto their back") {}
    SECTION("a heavy hit from behind knocks the player onto their face") {
        blow.direction.z = 1;
        expected = PlayerDeed::FallForward;
        first = Action::FallForward;
        recovery = Action::GetUpForward;
    }
    SECTION("a raised guard halves the hit and downgrades falling to recoil") {
        for (s32 i = 0; i < 60; ++i) {
            player.figure->animate(0, 2, 1.0f / 30, PlayerDeed::Defend);
        }
        REQUIRE(player.figure->animator().defending());
        remainingHealth = 995;
        expected = PlayerDeed::Flinch;
        first = Action::HitReact;
        recovery = Action::Ready;
    }
    SECTION("stun flags reel without knocking the player off their feet") {
        blow.flags = PlayerImpact::kStun;
        expected = PlayerDeed::Reel;
        first = Action::Stun;
        recovery = Action::Ready;
    }
    LevelOpponents::applyCritterBlow(blow, f.players, opponents);
    REQUIRE(player.actor.save().health() == remainingHealth);
    REQUIRE(player.reaction == expected);
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].attack = true;
    f.players[1].life = PlayerLife::InTower;
    f.events.advanceTurbo = [](usize, s32, f32) {};
    f.step();
    REQUIRE(player.figure->animator().action() == first);
    bool gotUp = false;
    for (s32 i = 0; i < 120 && player.figure->animator().reacting(); ++i) {
        f.step();
        REQUIRE(player.actor.position() == Vec3(0));
        REQUIRE_FALSE(player.figure->animator().released());
        REQUIRE(f.calls.empty());
        gotUp = gotUp || player.figure->animator().action() == recovery;
    }
    REQUIRE(gotUp);
    REQUIRE_FALSE(player.figure->animator().reacting());
    f.inputs[3].attack = false;
    f.step();
    REQUIRE(player.actor.position().z > 0);
}

TEST_CASE("boss capture owns the full body transform and defers throw damage until landing",
          "[game][screens][party-motion][yeti]") {
    Fixture f;
    auto& runtime = f.players[0];
    f.players[1].life = PlayerLife::InTower;
    f.inputs[3].move = MoveInput{Vec2{1, 0}, 1};
    f.inputs[3].attack = true;
    f.inputs[3].usePotion = true;
    const Mat4 hand = glm::rotate(glm::translate(Mat4{1}, Vec3{0, 10, 0}), 0.5f, Vec3{0, 0, 1});
    LevelOpponents::applyGrab({3, 0, hand, Vec3{0}, 0}, true, f.players);
    REQUIRE(runtime.capture.held());
    REQUIRE(runtime.capture.body().has_value());
    REQUIRE((*runtime.capture.body())[0] == hand[0]);
    const Vec3 heldPosition = runtime.actor.position();
    const s32 before = runtime.actor.save().health();
    f.step();
    REQUIRE(runtime.actor.position() == heldPosition);
    REQUIRE(f.calls.empty()); // walking, attacks, potions, turbo and selectors all suppressed
    // Another actor cannot steal a player or release somebody else's grip.
    LevelOpponents::applyGrab({3, 4, std::nullopt, Vec3{0, 0, 1000}, 100}, false, f.players);
    REQUIRE(runtime.capture.held());
    LevelOpponents::applyGrab({3, 0, std::nullopt, Vec3{0, -100, 1000}, 100}, true, f.players);
    REQUIRE_FALSE(runtime.capture.held());
    REQUIRE(runtime.capture.flying());
    REQUIRE(runtime.actor.save().health() == before);
    s32 impacts = 0;
    f.events.thrownImpact = [&](usize index, f32 damage) {
        REQUIRE(index == 0);
        REQUIRE(damage == 100);
        ++impacts;
    };
    f.step();
    REQUIRE(runtime.actor.position().z <= 40.0f / 30.0f);
    REQUIRE(impacts == 0);
    for (s32 frame = 0; runtime.capture.flying() && frame < 120; ++frame) {
        f.step();
    }
    REQUIRE_FALSE(runtime.capture.active());
    REQUIRE(runtime.actor.position().y == 0);
    REQUIRE(impacts == 1);
    f.step();
    REQUIRE(impacts == 1);
}

TEST_CASE("cancelled capture lowers safely and dead players do not retain attachments",
          "[game][screens][party-motion][yeti]") {
    Fixture f;
    auto& runtime = f.players[0];
    const Mat4 hand = glm::translate(Mat4{1}, Vec3{0, 8, 0});
    LevelOpponents::applyGrab({3, 0, hand, Vec3{0}, 0}, true, f.players);
    LevelOpponents::applyGrab({3, 0, std::nullopt, Vec3{0}, 0}, true, f.players);
    f.events.thrownImpact = [](usize, f32 damage) { REQUIRE(damage == 0); };
    for (s32 i = 0; runtime.capture.active() && i < 120; ++i) {
        f.step();
    }
    REQUIRE_FALSE(runtime.capture.active());
    REQUIRE(runtime.actor.position() == Vec3{0});
    LevelOpponents::applyGrab({3, 0, hand, Vec3{0}, 0}, true, f.players);
    runtime.life = PlayerLife::Dying;
    f.step();
    REQUIRE_FALSE(runtime.capture.active());
}

TEST_CASE("charge steering and strafe directions remain camera relative",
          "[game][screens][party-motion]") {
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, Vec3{0}, std::numbers::pi_v<f32> / 2);
    const auto straight = PartyMotion::chargeInput(actor, {}, 0);
    REQUIRE(straight.magnitude == 1);
    REQUIRE(straight.direction.x == Approx(1));
    const MoveInput pushed{Vec2{-1, 0}, 0.25f};
    REQUIRE(PartyMotion::chargeInput(actor, pushed, 0).direction == pushed.direction);
    REQUIRE(PartyMotion::strafeWayOf(0, 0) == StrafeWay::Forward);
    REQUIRE(PartyMotion::strafeWayOf(std::numbers::pi_v<f32>, 0) == StrafeWay::Back);
    REQUIRE(PartyMotion::strafeWayOf(1.0f, 0) == StrafeWay::Right);
    REQUIRE(PartyMotion::strafeWayOf(-1.0f, 0) == StrafeWay::Left);
}
TEST_CASE("held close attack input plants the feet and dispatches melee contacts instead of throws",
          "[game][screens][party-motion][melee][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    Fixture f;
    test::FakeRenderDevice device;
    f.players[0].figure = PlayerFigure::load(device, root, f.players[0].actor.save(), false);
    REQUIRE(f.players[0].figure);
    s32 contacts = 0;
    s32 missiles = 0;
    f.events.advanceTurbo = [](usize, s32, f32) {};
    f.events.attackDeed = [](usize, bool strong) {
        return strong ? PlayerDeed::MeleeSlow : PlayerDeed::Melee;
    };
    f.events.perform = [&](usize, PartyMotion::Action action) {
        contacts += action == PartyMotion::Action::Melee ? 1 : 0;
        missiles +=
            action == PartyMotion::Action::ThrowWeapon || action == PartyMotion::Action::StrongThrow
                ? 1
                : 0;
    };
    f.inputs[3].attack = true;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    for (s32 frame = 0; frame < 60; ++frame) {
        f.step();
        CHECK(f.players[0].actor.position() == Vec3{0});
    }
    CHECK(contacts >= 3);
    CHECK(missiles == 0);
    CHECK(f.players[0].figure->animator().meleeing());
}
} // namespace
