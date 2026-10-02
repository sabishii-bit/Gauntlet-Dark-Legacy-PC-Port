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
#include "game/players/PowerupEffects.h"
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
    std::vector<usize> fallen; ///< who the step reported gone
    PartyMotion::Events events{
        .perform =
            [this](usize i, PartyMotion::Action action) {
                if (action == PartyMotion::Action::Fallen) {
                    fallen.push_back(i);
                    return;
                }
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
        .limitMovement = {},
        .attackDeed = {},
        .meleeSense = {},
        .grabDeath = {},
        .resolveMovement = {},
        .startPoint = {},
        .comboImpact = {}};

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
    // With no figure to play its death, it is gone at once, and says so once.
    CHECK(f.players[0].life == PlayerLife::InTower);
    CHECK(f.fallen == std::vector<usize>{0});
    f.step();
    CHECK(f.fallen == std::vector<usize>{0});
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
    f.events.limitMovement = [](usize, const Vec3& before, const Vec3& after) {
        return after.z <= before.z ? after : before;
    };
    const auto before = f.players[0].actor.position();
    const auto subjects = f.step();
    REQUIRE(f.players[0].actor.position().z == before.z);
    REQUIRE(subjects[0].feet == f.players[0].actor.position());
    f.inputs[3].move.direction.y = -1;
    f.step();
    REQUIRE(f.players[0].actor.position().z < before.z);
}

TEST_CASE("quick melee creeps forward from its first input frame while slow melee stays planted",
          "[game][party-motion][melee]") {
    Fixture f;
    f.inputs[3].attack = true;
    f.inputs[3].move = MoveInput{Vec2{1, 0}, 1};
    f.events.attackDeed = [](usize, bool, bool) { return PlayerDeed::Melee; };
    f.step();
    const Vec3 quick = f.players[0].actor.position();
    CHECK(quick.z > 0);
    CHECK(quick.x == Approx(0).margin(0.0001f));
    CHECK(f.players[0].actor.yaw() == Approx(0));
    f.events.attackDeed = [](usize, bool, bool) { return PlayerDeed::MeleeSlow; };
    f.step();
    CHECK(f.players[0].actor.position() == quick);
}

TEST_CASE("camera clipping preserves a diagonal step along the screen edge",
          "[game][party-motion][camera-limit]") {
    Fixture f;
    f.inputs[3].move = MoveInput{glm::normalize(Vec2{1, 1}), 1};
    f.events.limitMovement = [](usize, const Vec3& before, const Vec3& after) {
        return Vec3{before.x, after.y, after.z};
    };
    const auto subjects = f.step();
    CHECK(f.players[0].actor.position().x == Approx(0));
    CHECK(f.players[0].actor.position().z > 0);
    CHECK(subjects[0].feet == f.players[0].actor.position());
}

TEST_CASE("party motion resolves creatures before camera limits and snapshots",
          "[game][party-motion][collision]") {
    Fixture f;
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.events.resolveMovement = [](usize, const Vec3& from, const Vec3& to) {
        return Vec3{from.x, to.y, from.z};
    };
    f.events.limitMovement = [](usize, const Vec3& from, const Vec3& to) {
        CHECK(from == to);
        return to;
    };
    const auto subjects = f.step();
    CHECK(f.players[0].actor.position() == Vec3{0});
    CHECK(subjects[0].feet == Vec3{0});
}

TEST_CASE("collision corrections cannot push a player through a level wall",
          "[game][party-motion][collision][push-wall]") {
    Fixture f;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-10, 0, -10}, Vec3{10, 0, -10}, Vec3{0, 0, 10}};
    floor.normal = Vec3{0, 1, 0};
    CollisionTriangle wall;
    wall.vertices = {Vec3{1, 0, -10}, Vec3{1, 10, -10}, Vec3{1, 0, 10}};
    wall.normal = Vec3{-1, 0, 0};
    f.collision.build({floor, wall});
    SECTION("large creature or fixture correction crosses a thin wall") {
        f.events.resolveMovement = [](usize i, const Vec3&, const Vec3& to) {
            return i == 0 ? to + Vec3{4, 0, 0} : to;
        };
    }
    SECTION("another party member crowds the player against the wall") {
        f.players[1].actor.place(Vec3{-0.5f, 0, 0});
    }
    const auto subjects = f.step();
    CHECK(f.players[0].actor.position().x <= 1 - f.players[0].actor.radius() + 0.001f);
    CHECK(f.players[0].actor.position().y == Approx(0));
    CHECK(subjects[0].feet == f.players[0].actor.position());
}

TEST_CASE("creature corrections stay on the Desert entrance walkway",
          "[game][party-motion][collision][push-wall][unpacked]") {
    const auto dir = test::unpackedOrSkip("LEVELS/LEVELC1/collision.json").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    Fixture f;
    REQUIRE(f.collision.load(dir, layout));
    f.players[1].life = PlayerLife::InTower;
    // The first authored General stands on this narrow bridge, between the start
    // and the main island. Both lateral pushes must preserve the walkable surface.
    for (const f32 direction : {-1.0f, 1.0f}) {
        f.players[0].actor.place(Vec3{-60.875f, 0.7265625f, 29.8125f});
        f.players[0].actor.settle(f.collision);
        const f32 startHeight = f.players[0].actor.position().y;
        f.events.resolveMovement = [direction](usize i, const Vec3&, const Vec3& to) {
            return i == 0 ? to + Vec3{direction * 4, 0, 0} : to;
        };
        for (s32 tick = 0; tick < 30; ++tick) {
            f.step();
            const Vec3 at = f.players[0].actor.position();
            CAPTURE(direction, tick, at.x, at.y, at.z);
            REQUIRE(at.y >= startHeight - 0.1f);
            REQUIRE(f.collision.floorAt(at, 1.5f, 3, PlayerActor::kFloorEdgeReach).has_value());
        }
    }
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
                                            .named = [](std::string_view, f32) {},
                                            .learnBlock = {}};
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
    // The stick moves nothing until the player is up; only the knock slides the body, the
    // way the blow travelled and no further than its kick carries.
    const f32 along = blow.flags == PlayerImpact::kStun ? 0.0f : blow.direction.z;
    bool gotUp = false;
    for (s32 i = 0; i < 120 && player.figure->animator().reacting(); ++i) {
        f.step();
        const Vec3 at = player.actor.position();
        REQUIRE(at.x == 0.0f);
        REQUIRE(at.z * along >= 0.0f);
        REQUIRE(std::abs(at.z) < 4.0f);
        if (along == 0.0f) {
            REQUIRE(at == Vec3(0));
        }
        REQUIRE_FALSE(player.figure->animator().released());
        REQUIRE(f.calls.empty());
        gotUp = gotUp || player.figure->animator().action() == recovery;
    }
    REQUIRE(gotUp);
    REQUIRE_FALSE(player.figure->animator().reacting());
    f.inputs[3].attack = false;
    const f32 settled = player.actor.position().z;
    f.step();
    REQUIRE(player.actor.position().z > settled);
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
TEST_CASE("held close attack input advances slowly and dispatches melee contacts instead of throws",
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
    f.events.attackDeed = [](usize, bool strong, bool) {
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
        const Vec3 before = f.players[0].actor.position();
        f.step();
        CHECK(f.players[0].actor.position().z > before.z);
        CHECK(f.players[0].actor.position().x == 0);
    }
    CHECK(contacts >= 3);
    CHECK(missiles == 0);
    CHECK(f.players[0].figure->animator().meleeing());
}

TEST_CASE("a knock turns its victim at once and slides it from the next frame",
          "[game][screens][party-motion][knockback]") {
    Fixture f;
    f.players[0].knockback.queue(Vec3{1, 0, 0}, Knockback::kKnockDown, 10.0f);
    f.step();
    CHECK(f.players[0].actor.yaw() == Approx(std::numbers::pi_v<f32> / 2));
    CHECK(f.players[0].actor.position().x == Approx(0.0f));
    f.step();
    CHECK(f.players[0].actor.position().x == Approx(32.0f / 30.0f));
    for (s32 frame = 0; frame < 30; ++frame) {
        f.step();
    }
    const f32 rest = f.players[0].actor.position().x;
    CHECK(rest < 4.0f);
    f.step();
    CHECK(f.players[0].actor.position().x == Approx(rest)); // the slide is over
    // The fallen are not pushed.
    f.players[1].life = PlayerLife::InTower;
    f.players[1].knockback.queue(Vec3{1, 0, 0}, Knockback::kKnockDown, 10.0f);
    f.step();
    f.step();
    CHECK(f.players[1].actor.position().x == Approx(10.0f));
}

TEST_CASE("a body lost under the world stands again beside another, or at the start",
          "[game][screens][party-motion][falling]") {
    Fixture f;
    // Held above its floor, it sinks at sixteen a second.
    f.players[0].actor.place(Vec3{0, 2, 0});
    f.step();
    CHECK(f.players[0].actor.position().y == Approx(2.0f - 16.0f / 30.0f));
    // Out past the floor's edge, lost under it: stood again round the other player.
    f.players[1].actor.place(Vec3{500, -10, 0});
    f.players[0].actor.place(Vec3{0, 0, 0});
    f.step();
    const Vec3 rescued = f.players[1].actor.position();
    CHECK(rescued.y == 0.0f);
    CHECK(glm::distance(rescued, Vec3{0, 0, 0}) ==
          Approx(PartyMotion::kRescueGap + 2 * f.players[0].actor.radius()));
    // With nobody to stand beside, the level's start.
    f.players[0].life = PlayerLife::InTower;
    f.players[1].actor.place(Vec3{500, -10, 0});
    f.events.startPoint = [] { return std::optional<Vec3>{Vec3{7, 0, 7}}; };
    f.step();
    CHECK(f.players[1].actor.position() == Vec3{7, 0, 7});
    CHECK_FALSE(PartyMotion::rescueSpot(f.players, 1, f.collision).has_value());
}
TEST_CASE("it passes to a player touched once it has been held a second, and not past a fall",
          "[game][screens][party-motion][it]") {
    Fixture f;
    std::vector<usize> tagged;
    f.events.perform = [&](usize i, PartyMotion::Action action) {
        if (action == PartyMotion::Action::Tagged) {
            tagged.push_back(i);
        }
    };
    f.players[0].itTicks = 1;
    f.players[1].actor.place(Vec3{0.5f, 0.0f, 0.0f});
    PartyMotion::passIt(f.players, 2, f.events);
    CHECK(tagged.empty()); // held too short a while to pass on
    CHECK(f.players[0].itTicks == 3);
    f.players[0].itTicks = PartyMotion::kItHold + 1;
    PartyMotion::passIt(f.players, 2, f.events);
    CHECK(tagged == std::vector<usize>{1});
    CHECK(f.players[0].itTicks == 0);
    CHECK(f.players[1].itTicks == 3);
    // Apart, it stays where it is.
    f.players[1].actor.place(Vec3{10.0f, 0.0f, 0.0f});
    f.players[1].itTicks = 100;
    PartyMotion::passIt(f.players, 2, f.events);
    CHECK(tagged.size() == 1);
    CHECK(f.players[1].itTicks == 102);
    // The fallen are it no more.
    f.players[1].life = PlayerLife::InTower;
    PartyMotion::passIt(f.players, 2, f.events);
    CHECK(f.players[1].itTicks == 0);
}

TEST_CASE("a body retches while gas lasts with the stick let go, heeding no button; a pickup's "
          "gesture waits for nothing else to be asked",
          "[game][screens][party-motion][pickup]") {
    Fixture f;
    f.players[0].gagSeconds = 0.5f;
    f.inputs[3].attack = true;
    f.step();
    const auto selected = [&] { return std::ranges::find(f.calls, "select0") != f.calls.end(); };
    CHECK_FALSE(selected()); // retching: the buttons are not read
    CHECK(f.players[0].gagSeconds == Approx(0.5f - 1.0f / 30.0f));
    // Moving, it does not retch, and the buttons are read again.
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.step();
    CHECK(selected());
    f.calls.clear();
    f.inputs[3] = {};
    f.players[0].gagSeconds = 0.0f;
    f.players[0].gesture = PlayerDeed::Pick;
    f.players[0].reaction = PlayerDeed::Flinch;
    f.step();
    CHECK(f.players[0].gesture == PlayerDeed::Pick); // kept while something else is asked
    f.step();
    CHECK(f.players[0].gesture == PlayerDeed::None);
}

TEST_CASE("a halo wearer holding Death stands facing him and heeds no button",
          "[game][screens][party-motion][death]") {
    Fixture f;
    std::vector<bool> allowed;
    f.events.grabDeath = [&](usize i, s32, bool may) -> std::optional<Vec3> {
        if (i != 0) {
            return std::nullopt;
        }
        allowed.push_back(may);
        return Vec3{10, 0, 0};
    };
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    f.inputs[3].attack = true;
    f.step();
    CHECK(f.players[0].actor.position() == Vec3{0, 0, 0});                  // no step taken
    CHECK(f.players[0].actor.yaw() == Approx(std::numbers::pi_v<f32> / 2)); // facing him
    CHECK(std::ranges::find(f.calls, "select0") == f.calls.end());
    f.step(true);
    CHECK(allowed == std::vector<bool>{true, false}); // held, no hold may be made
}

TEST_CASE("a member walking into another is stopped and shoves them along",
          "[game][screens][party-motion][party-collision]") {
    Fixture f;
    f.players[1].actor.place(Vec3{0, 0, 3});
    const f32 reach = f.players[0].actor.radius() + f.players[1].actor.radius();
    f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
    for (s32 i = 0; i < 30; ++i) {
        f.step();
        const f32 apart =
            glm::distance(f.players[0].actor.position(), f.players[1].actor.position());
        CHECK(apart >= reach - 0.01f);
    }
    // It yields to the shoves, slowly, and knows it is pushed.
    CHECK(f.players[1].actor.position().z > 3.0f);
    CHECK(f.players[1].actor.position().z < 3.0f + 30.0f * f.players[1].actor.speed() / 30.0f);
    CHECK(f.players[1].knockback.pushed());
    CHECK_FALSE(f.players[0].knockback.pushed());
    // Let go of, the shove fades.
    f.inputs[3].move = MoveInput{};
    for (s32 i = 0; i < 30; ++i) {
        f.step();
    }
    CHECK_FALSE(f.players[1].knockback.pushed());
    CHECK_FALSE(f.players[1].knockback.sliding());
}

TEST_CASE("a levitating body walks without a footfall", "[game][screens][party-motion][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    for (const bool levitating : {false, true}) {
        CAPTURE(levitating);
        Fixture f;
        test::FakeRenderDevice device;
        f.players[0].figure = PlayerFigure::load(device, root, f.players[0].actor.save(), false);
        REQUIRE(f.players[0].figure);
        auto& inventory = f.players[0].actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 60);
        inventory.powerups[0].on = levitating;
        s32 footfalls = 0;
        f.events.advanceTurbo = [](usize, s32, f32) {};
        f.events.perform = [&](usize, PartyMotion::Action action) {
            footfalls += action == PartyMotion::Action::FirstFoot ||
                                 action == PartyMotion::Action::SecondFoot
                             ? 1
                             : 0;
        };
        f.inputs[3].move = MoveInput{Vec2{0, 1}, 1};
        for (s32 frame = 0; frame < 120; ++frame) {
            f.step();
        }
        CHECK(f.players[0].actor.position().z > 0);
        CHECK((footfalls > 0) == !levitating);
    }
}

TEST_CASE("the combo button with half the meter lifts a warrior's partner ahead, flings it at "
          "frame thirty and lets it go four seconds on",
          "[game][screens][party-motion][combo][unpacked]") {
    const auto root = test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    Fixture f;
    test::FakeRenderDevice device;
    for (PlayerRuntime& player : f.players) {
        player.figure = PlayerFigure::load(device, root, player.actor.save(), false);
        REQUIRE(player.figure);
    }
    f.players[1].actor.place(Vec3{0, 0, 3}); // three ahead of player 3's warrior, facing +z
    std::vector<std::string> impacts;
    f.events.advanceTurbo = [](usize, s32, f32) {};
    f.events.perform = [&](usize i, PartyMotion::Action action) {
        if (action == PartyMotion::Action::ComboStart) {
            f.calls.push_back("combo" + std::to_string(i));
        }
    };
    f.events.comboImpact = [&](usize flier, usize thrower, f32 blow) {
        impacts.push_back(std::to_string(flier) + "by" + std::to_string(thrower) + "for" +
                          std::to_string(static_cast<s32>(blow)));
        return false;
    };
    f.inputs[3].combo = true;
    // Without half the meter nothing happens; with it the pair are tied.
    f.step();
    CHECK_FALSE(f.players[0].combo.active());
    f.players[0].turbo.add(ComboMove::kMeterNeeded);
    f.step();
    REQUIRE(f.players[0].combo.role == ComboRole::Grabber);
    REQUIRE(f.players[1].combo.role == ComboRole::Held);
    CHECK(f.calls == std::vector<std::string>{"select0", "select1", "combo0", "select0"});
    CHECK(f.players[0].figure->animator().action() == PlayerAnimator::Action::ComboAct1);
    CHECK(f.players[1].combo.riding);
    // The partner hangs on the warrior's DUMMY node (which rides the root's own motion
    // through the move) and plays COMBOWAR1, heeding nothing.
    f.inputs[1].move = MoveInput{Vec2{1, 0}, 1};
    f.inputs[1].attack = true;
    f.step();
    CHECK(f.players[1].figure->animator().action() == PlayerAnimator::Action::ComboWar1);
    CHECK(f.players[1].actor.position().x == Approx(0.0f).margin(0.5f));
    CHECK(glm::distance(f.players[1].actor.position(), f.players[0].actor.position()) < 4.0f);
    // Frame thirty lets it fly along the warrior's facing; the warrior's move plays on.
    s32 frames = 2;
    while (f.players[1].combo.role == ComboRole::Held && frames < 60) {
        f.step();
        ++frames;
    }
    REQUIRE(f.players[1].combo.role == ComboRole::Thrown);
    CHECK(frames == 31);
    CHECK_FALSE(f.players[1].combo.riding);
    CHECK(ComboMove::flies(f.players[1].combo));
    const f32 before = f.players[1].actor.position().z;
    f.step();
    CHECK(f.players[1].figure->animator().action() == PlayerAnimator::Action::ComboWar2);
    CHECK(f.players[1].actor.position().z ==
          Approx(before + ComboMove::kFlightSpeed / 30.0f).margin(0.01f));
    CHECK(impacts.back() == "1by0for50");
    // Four seconds on the pair are let go of; the pinball lands and stands again.
    s32 ticks = 2; // the step just taken flew too
    while (f.players[1].combo.active() && ticks < 600) {
        f.step();
        ticks += 2;
    }
    CHECK(ticks == ComboMove::kFlightTicks);
    CHECK_FALSE(f.players[0].combo.active());
    CHECK(f.players[1].figure->animator().action() == PlayerAnimator::Action::ComboWar3);
    for (s32 i = 0; i < 30; ++i) {
        f.step();
    }
    CHECK(f.players[1].figure->animator().action() != PlayerAnimator::Action::ComboWar3);
    CHECK(f.players[1].actor.position().x > 0.5f); // its own stick again
}
} // namespace
