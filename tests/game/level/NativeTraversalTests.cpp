#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/level/Chests.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/level/PlacedItems.h"
#include "game/party/FloorRiding.h"
#include "game/party/PartyPickups.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerActor.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

struct NativeWalk {
    test::FakeRenderDevice device;
    LevelWorld world;
    std::array<PlayerRuntime, 1> party;
    Vec3 carried{0};
    Vec3 travelled{0};
    Vec3 landed{0};
    s32 ownershipDiagnostics = 0;
    f32 seconds = kStep;

    NativeWalk(std::string_view level, Vec3 start, f32 stepSeconds = kStep) : seconds(stepSeconds) {
        const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
        LevelCatalog catalog;
        REQUIRE(catalog.load(root));
        const auto info = catalog.byName(level);
        REQUIRE(info.has_value());
        REQUIRE(world.load(device, root, *info));
        world.startTriggers({});
        world.updateTriggers(0, {});
        ClassDataSet classes;
        REQUIRE(classes.load(root / "PDATA"));
        const auto* stats = classes.stats(0);
        REQUIRE(stats != nullptr);
        party[0].actor.spawn(0, {}, stats, start, 0);
        party[0].actor.settle(world.collision());
        FloorRiding::land(party, 0, party[0].actor.position(), world.collision());
    }

    void step(Vec2 direction = Vec2{0}) {
        auto& actor = party[0].actor;
        world.update(seconds);
        FloorRiding::carry(party[0], world.collision());
        carried = actor.position();
        const Vec3 before = actor.position();
        actor.clearWallContacts();
        actor.update(MoveInput{.direction = direction,
                               .magnitude = glm::length(direction) > 0 ? 1.0f : 0.0f},
                     0, seconds, &world.collision());
        travelled = actor.position();
        const auto centre =
            world.collision().floorAt(travelled, FloorRiding::kProbeAbove, FloorRiding::kProbeBelow,
                                      PlayerActor::kFloorEdgeReach);
        const auto footprint = world.collision().floorAt(travelled, PlayerActor::kFloorEdgeReach,
                                                         FloorRiding::kProbeBelow, actor.radius());
        const s32 previousFloor = party[0].floor.object;
        FloorRiding::land(party, 0, before, world.collision());
        landed = actor.position();
        if (centre && footprint && centre->object != party[0].floor.object &&
            ownershipDiagnostics < 8) {
            ++ownershipDiagnostics;
            UNSCOPED_INFO("support choice from "
                          << previousFloor << " carry(" << carried.x << "," << carried.y << ","
                          << carried.z << ") walk(" << travelled.x << "," << travelled.y << ","
                          << travelled.z << ") centre " << centre->object << " at " << centre->y
                          << " footprint " << footprint->object << " at " << footprint->y
                          << " chose " << party[0].floor.object);
        }
        actor.fall(seconds, world.collision());
        const std::array visitors{TriggerVisitor{.position = actor.position(),
                                                 .radius = actor.reach(), // as PlayScene has it
                                                 .floorObject = party[0].floor.object,
                                                 .height = actor.height()}};
        world.updateTriggers(seconds, visitors);
    }

    void walk(Vec2 goal) {
        const auto& actor = party[0].actor;
        for (s32 frame = 0; frame < static_cast<s32>(8.0f / seconds); ++frame) {
            const Vec2 delta = goal - Vec2{actor.position().x, actor.position().z};
            if (glm::length(delta) < 0.5f) {
                return;
            }
            step(glm::normalize(delta));
        }
        CAPTURE(goal.x, goal.y, actor.position().x, actor.position().y, actor.position().z,
                party[0].floor.object);
        const Vec3 delta{goal.x - actor.position().x, 0, goal.y - actor.position().z};
        const Vec3 target = actor.position() + glm::normalize(delta) * (actor.speed() * seconds);
        const auto direct = world.collision().floorAt(target, 3, 3, actor.radius());
        if (direct) {
            UNSCOPED_INFO("reachable footprint floor " << direct->object << " at " << direct->y);
        }
        const auto cleared = world.collision().resolveWalls(
            actor.position(), actor.radius(), actor.position().y + PlayerActor::kFootClearance,
            actor.position().y + actor.height() - PlayerActor::kFootClearance);
        UNSCOPED_INFO("overlap resolution (" << cleared.x << ", " << cleared.y << ", " << cleared.z
                                             << ")");
        for (const auto& contact : actor.wallContacts()) {
            UNSCOPED_INFO("blocking object " << contact.object << " at " << contact.point.x << ", "
                                             << contact.point.y << ", " << contact.point.z);
        }
        FAIL("native walking route did not reach its waypoint");
    }

    void wait(s32 frames) {
        for (s32 frame = 0; frame < frames; ++frame) {
            step();
        }
    }

    void stand(Vec3 position, s32 character, const ClassStats& stats) {
        CharacterSave save;
        save.selectClass(character);
        party[0].floor = {};
        party[0].actor.spawn(0, save, &stats, position, 0);
        party[0].actor.settle(world.collision());
        FloorRiding::land(party, 0, party[0].actor.position(), world.collision());
    }
};

TEST_CASE("Ice Camp railway ledge contacts do not strand a player outside their floor",
          "[native-traversal][ice-rail-ledge][assets]") {
    const s32 hz = GENERATE(30, 60, 120);
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    // CAVECARTE#05, the railway tunnel below the starting area. A footprint
    // reached the narrow side ledge (1211) without its centre ever reaching
    // that floor, then every reverse step was rejected. Each class walks a fresh
    // level, so the tunnel's cart meets none of them.
    for (s32 character = 0; character < kClassCount; ++character) {
        NativeWalk walk("I2", {-51.0781f, 34.0156f, 79.7188f}, 1.0f / static_cast<f32>(hz));
        const auto* stats = classes.stats(character == kSumnerClass ? kWizardClass : character);
        REQUIRE(stats);
        walk.stand({-51.0781f, 34.0156f, 79.7188f}, character, *stats);
        const Vec3 start = walk.party[0].actor.position();
        for (s32 tick = 0; tick < hz / 2; ++tick) {
            walk.step({0, -1});
        }
        const Vec3 end = walk.party[0].actor.position();
        for (s32 tick = 0; tick < hz; ++tick) {
            walk.step({0, 1});
        }
        const Vec3 returned = walk.party[0].actor.position();
        CAPTURE(hz, character, start.x, start.y, start.z, end.x, end.y, end.z, returned.x,
                returned.y, returned.z);
        CHECK(returned.z > start.z - 0.3f);
        CHECK(returned.y <= start.y + 0.1f);
    }
}

TEST_CASE("Ice Camp tunnel side strip can rejoin the sloping railroad",
          "[native-traversal][ice-rail-reboard][assets]") {
    const s32 hz = GENERATE(30, 60, 120);
    const bool maximumSpeed = GENERATE(false, true);
    const Vec3 start{43.234375f, 28.8499f, 50.8984375f};
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    // CAVECARTE#08: leave side strip I2#086 for the raised rail deck I2#087.
    // The rail has a long side triangle whose distant high end used to admit
    // a knee-height wall contact below the player's native collision volume.
    for (s32 character = 0; character < kClassCount; ++character) {
        NativeWalk walk("I2", start, 1.0f / static_cast<f32>(hz));
        const auto* stats = classes.stats(character == kSumnerClass ? kWizardClass : character);
        REQUIRE(stats);
        CAPTURE(hz, character, maximumSpeed);
        walk.stand(start, character, *stats);
        if (maximumSpeed) {
            walk.party[0].actor.setPaceBonus(PlayerActor::kMaxSpeed);
        }
        walk.walk({start.x, start.z - 2});
        CHECK(walk.party[0].actor.position().y > 29.8f);
        // Clear the rail with the whole footprint before checking descent.
        walk.walk({start.x, start.z + 1});
        walk.wait(hz / 2);
        CHECK(walk.party[0].actor.position().y < 29.1f);
    }
}

TEST_CASE("G1 first lift can be boarded from its lower landing and left above",
          "[native-traversal][g1-first-lift][assets]") {
    const f32 offset = GENERATE(-2.0f, 0.0f, 2.0f);
    const s32 hz = GENERATE(30, 60);
    CAPTURE(offset, hz);
    NativeWalk walk("G1", {41, 10, -85 + offset}, 1.0f / static_cast<f32>(hz));
    walk.walk({48.4f, -85 + offset});
    REQUIRE(walk.party[0].floor.object == 1083);
    walk.wait(3 * hz);
    REQUIRE(walk.party[0].actor.position().y > 19);
    walk.walk({55, -85 + offset});
    CHECK(walk.party[0].floor.object != 1083);
}

TEST_CASE("G1 first lift riders can leave a pressed corner",
          "[native-traversal][g1-lift-corners][assets]") {
    const f32 side = GENERATE(-1.0f, 1.0f);
    const f32 end = GENERATE(-1.0f, 1.0f);
    const s32 delay = GENERATE(0, 75);
    const s32 hz = GENERATE(30, 60);
    CAPTURE(side, end, delay, hz);
    NativeWalk walk("G1", {41, 10, -85}, 1.0f / static_cast<f32>(hz));
    walk.walk({48.4f, -85});
    walk.wait(delay * (hz / 30));
    for (s32 frame = 0; frame < 3 * hz; ++frame) {
        walk.step(glm::normalize(Vec2{side, end}));
    }
    walk.walk({48.4f, -85});
    CHECK(walk.party[0].floor.object == 1083);
}

TEST_CASE("G1 second lift can descend and recover from its broken fence corner",
          "[native-traversal][g1-second-lift][assets]") {
    const f32 end = GENERATE(-1.0f, 1.0f);
    const s32 hz = GENERATE(30, 60);
    CAPTURE(end, hz);
    NativeWalk walk("G1", {-24.375f, 27.3f, -156.71875f}, 1.0f / static_cast<f32>(hz));
    walk.wait(3 * hz);
    REQUIRE(walk.world.triggers().opened(273));
    walk.walk({-24.375f, -152});
    walk.walk({-17.75f, -152});
    REQUIRE(walk.party[0].floor.object == 273);
    bool brokeFence = false;
    for (usize i = 0; i < walk.world.walls().size(); ++i) {
        const Vec3 position{walk.world.walls().wall(i).transform[3]};
        if (glm::distance(position, Vec3{-21.9765625f, 20.171875f, -154.5625f}) < 0.1f) {
            REQUIRE(walk.world.strikeWall(i, 1000) == 0);
            brokeFence = true;
        }
    }
    REQUIRE(brokeFence);
    for (s32 frame = 0; frame < 3 * hz; ++frame) {
        walk.step(glm::normalize(Vec2{-1, end}));
    }
    // Pressed into either west corner while the deck descends, the body is held on the
    // lift until it stops (PlayerCollideFloor refuses every other floor while the lift's
    // flags hold), then its corner step takes it the one to two units down onto the
    // ground west of the lift. In the south-west corner it meets fence 1187
    // (x=-27.05..-21.73 at z=-157.2, y=18.1..23.2) and the cage wall a width apart: a wall
    // it already overlaps pushes it out rather than holding it. Either way it stands free
    // on the ground; the rim and the fence leave no way of its width back onto the deck.
    for (s32 frame = 0; frame < hz / 3; ++frame) {
        walk.step({0, -1});
    }
    CHECK(walk.party[0].floor.object != 273);
    walk.walk({-24, -145});
}

TEST_CASE("G2 entrance bridge can be crossed both ways by every class",
          "[native-traversal][g2-entrance-bridge][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const f32 offset = GENERATE(-2.0f, 0.0f, 2.0f);
    NativeWalk walk("G2", {-15 + offset, 0, 68});
    for (s32 character = 0; character < kClassCount; ++character) {
        CAPTURE(character, offset);
        const auto* stats = classes.stats(character == kSumnerClass ? 2 : character);
        REQUIRE(stats != nullptr);
        walk.stand({-15 + offset, 0, 68}, character, *stats);
        walk.walk({-12 + offset, 50});
        // The far fence narrows toward z=30. At (-12, 33), its plane is only
        // 0.991 from the native sphere centre, inside the 1.5 radius. Converge
        // to the clear exit lane rather than demand penetration of that fence.
        walk.walk({-14 + offset * 0.5f, 33});
        walk.walk({-12 + offset, 50});
        walk.walk({-15 + offset, 68});
    }
}

TEST_CASE("G2 drawbridge 723 opens its lower passage for every class",
          "[native-traversal][g2-drawbridge][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const bool opened = GENERATE(false, true);
    const f32 offset = GENERATE(-1.0f, 0.0f, 1.0f);
    CAPTURE(opened, offset);
    // G2ELEV1 is object 723, not entrance bridge 1429. Its rest mesh slopes
    // from y=.6640625 to -5.4921875. The initial -.602139 roll holds it level;
    // trigger item 420 at this spot lowers it into the passage beneath floor 1430.
    NativeWalk walk("G2", opened ? Vec3{3.6328125f, 0.0234375f, 0.9765625f}
                                 : Vec3{-28, 0.6640625f, -11.6484375f});
    walk.wait(95);
    REQUIRE(walk.world.triggers().opened(723) == opened);
    REQUIRE(walk.world.collision().solid(723));
    const f32 z = -11.6484375f + offset;
    for (s32 character = 0; character < kClassCount; ++character) {
        CAPTURE(character);
        const auto* stats = classes.stats(character == kSumnerClass ? 2 : character);
        REQUIRE(stats);
        walk.stand({-28, 0.6640625f, z}, character, *stats);
        REQUIRE(walk.party[0].floor.object == 1430);
        walk.walk({-21, z});
        REQUIRE(walk.party[0].floor.object == 723);
        if (opened) {
            walk.walk({-12, z});
            CHECK(walk.party[0].floor.object == 1385);
            CHECK(walk.party[0].actor.position().y < -5);
        } else {
            // The upper east lip is enclosed by wall 1623 at x=-14.63. The
            // switch lowers the bridge UNDER it; the initial pose is no exit.
            walk.walk({-16, z});
            for (s32 frame = 0; frame < 60; ++frame) {
                walk.step({1, 0});
            }
            CHECK(walk.party[0].actor.position().x < -14.6f);
            CHECK(walk.party[0].floor.object == 723);
            CHECK_FALSE(walk.party[0].actor.wallContacts().empty());
        }
        walk.walk({-21, z});
        REQUIRE(walk.party[0].floor.object == 723);
        walk.walk({-28, z});
        CHECK(walk.party[0].floor.object == 1430);
    }
}

TEST_CASE("G4 exit stairway reaches the circular portal platform",
          "[native-traversal][g4-exit-stairs][assets]") {
    NativeWalk walk("G4", {-6.5f, -5.90625f, -97.125f});
    walk.wait(45);
    REQUIRE(walk.world.triggers().opened(607));
    walk.walk({15, -94});
    walk.walk({27, -98});
    walk.wait(45);
    for (usize i = 0; i < walk.world.triggers().size(); ++i) {
        const auto& trigger = walk.world.triggers().trigger(i);
        if (trigger.target == 607 || trigger.target == 608) {
            UNSCOPED_INFO("exit trigger " << trigger.instance << " at " << trigger.spot.x << ","
                                          << trigger.spot.y << "," << trigger.spot.z << " flags "
                                          << trigger.flags << " floor " << trigger.floor
                                          << " fired " << trigger.fired);
        }
    }
    REQUIRE(walk.world.triggers().opened(608));
    walk.walk({15, -94});
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    for (s32 character = 0; character < kClassCount; ++character) {
        CAPTURE(character);
        const auto* stats = classes.stats(character == kSumnerClass ? 2 : character);
        REQUIRE(stats);
        walk.stand({8, -5.90625f, -99.75f}, character, *stats);
        CAPTURE(stats->width, stats->collisionY, stats->height);
        walk.walk({12.2265625f, -99.75f});
        walk.walk({17.25f, -104.875f});
        CHECK(walk.party[0].floor.object == 608);
    }
}

TEST_CASE("G4 entrance bridge riders can leave either railing and its joints",
          "[native-traversal][g4-bridge-rails][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const f32 side = GENERATE(-1.0f, 1.0f);
    CAPTURE(side);
    NativeWalk walk("G4", {-109.906f, -6.1875f, -62.129f});
    const Vec2 tangent{0.849f, 0.529f};
    const Vec2 outward = side * Vec2{0.529f, -0.849f};
    for (s32 character = 0; character < kClassCount; ++character) {
        CAPTURE(character);
        const auto* stats = classes.stats(character == kSumnerClass ? 2 : character);
        REQUIRE(stats);
        walk.stand({-109.906f, -6.1875f, -62.129f}, character, *stats);
        for (const Vec2 centre : {Vec2{-105.684f, -59.527f}, Vec2{-100.566f, -56.344f}}) {
            walk.walk(centre);
            bool touchedRail = false;
            for (s32 frame = 0; frame < 30; ++frame) {
                walk.step(outward);
                for (const auto& contact : walk.party[0].actor.wallContacts()) {
                    touchedRail |= contact.object == 1098;
                }
            }
            for (s32 frame = 0; frame < 30; ++frame) {
                walk.step(glm::normalize(tangent + outward * 0.5f));
            }
            REQUIRE(touchedRail);
            walk.walk(centre);
        }
        walk.walk({-95.391f, -53.121f});
        walk.walk({-90.883f, -50.262f});
        walk.walk({-109.906f, -62.129f});
    }
}

TEST_CASE("G3 fountain scenario settles on the completion pad and lowers the fire fences",
          "[native-traversal][g3-fountain][assets]") {
    // Focused completion fixture, behind animated trap wall G3TRAPW7. The
    // scenario is not an end-to-end traversal of the fountain puzzle.
    NativeWalk walk("G3", {-24.5625f, -20, -222.0859375f});
    REQUIRE(walk.party[0].floor.object == 1593);
    walk.wait(90);
    for (const s32 fence : {1313, 1323, 1326, 1335}) {
        CHECK(walk.world.triggers().opened(fence));
    }
}

/** The trigger that drives a level object, as the native levels register one a target. */
usize triggerOf(const LevelTriggers& triggers, s32 target) {
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).target == target) {
            return i;
        }
    }
    return triggers.size();
}

/** D3ELEV2 (446), which trigger item 46 raises ten units, its stone D3FLOOR_SMA#02 (447)
 * rising beside the walkway stone D3FLOOR_SMA#13. */
constexpr s32 kD3Lift = 446;
constexpr s32 kD3Stone = 447;
constexpr s32 kD3Walkway = 1225;
constexpr f32 kD3Rise = 10;

/** Sets the D3 stone rising for a party of one: pressed, it goes on the next update. */
void raiseD3Stone(NativeWalk& walk) {
    const LevelTriggers& triggers = walk.world.triggers();
    const usize pad = triggerOf(triggers, kD3Lift);
    REQUIRE(pad < triggers.size());
    walk.world.setPlayerCount(1);
    REQUIRE(walk.world.pressTrigger(pad));
    walk.step();
    REQUIRE(triggers.opened(kD3Lift));
    REQUIRE_FALSE(triggers.settled(kD3Lift));
}

TEST_CASE("D3 stepping stone rises to the walkway carrying a rider on its stone",
          "[native-traversal][d3-stepping-stones][assets]") {
    // did_generate (0x80063854) counts a player standing on the lift itself, not on the stone
    // hung from it, so the stone carries its rider all the way up.
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(0);
    REQUIRE(stats != nullptr);
    const s32 hz = GENERATE(30, 60);
    CAPTURE(hz);
    NativeWalk walk("D3", Vec3{0}, 1.0f / static_cast<f32>(hz));
    REQUIRE(walk.world.layout().objects()[kD3Stone].name == "D3FLOOR_SMA#02");
    const Vec3 bottom{walk.world.scene().worldTransform(kD3Stone)[3]};
    walk.stand(bottom, 0, *stats);
    REQUIRE(walk.party[0].floor.object == kD3Stone);
    REQUIRE((walk.party[0].floor.flags & FloorRiding::kMoving) != 0);
    raiseD3Stone(walk);
    const LevelTriggers& triggers = walk.world.triggers();
    for (s32 frame = 0; frame < 5 * hz && !triggers.settled(kD3Lift); ++frame) {
        walk.step();
        CHECK(walk.party[0].floor.object == kD3Stone);
    }
    REQUIRE(triggers.settled(kD3Lift));
    const Vec3 top{walk.world.scene().worldTransform(kD3Stone)[3]};
    CHECK(top.y == Approx(bottom.y + kD3Rise));
    walk.wait(hz / 2);
    CHECK(walk.party[0].floor.object == kD3Stone);
    CHECK(walk.party[0].actor.position().y == Approx(top.y).margin(0.01f));
}

TEST_CASE("D3 stepping stone cannot be stepped onto from the walkway until it stops rising",
          "[native-traversal][d3-stepping-stones][assets]") {
    // Rising, D3ELEV2 and its stone are open to the players' own probes (ProcessItemWobjs'
    // WorldObj +0x35 and WorldCollide's direct-parent OR, 0x8000D578), so a player on the
    // walkway pushing toward the stone cannot step down onto it in its last units of travel
    // and hold it short of the top: it reaches the top, and only then can the player step on.
    constexpr Vec3 kWalkway{-111.8f, 72.5f, 0}; // on D3FLOOR_SMA#13, north of the stone
    constexpr Vec2 kTowardStone{0, -1};
    const s32 hz = GENERATE(30, 60);
    CAPTURE(hz);
    NativeWalk walk("D3", kWalkway, 1.0f / static_cast<f32>(hz));
    REQUIRE(walk.world.layout().objects()[kD3Walkway].name == "D3FLOOR_SMA#13");
    REQUIRE(walk.party[0].floor.object == kD3Walkway);
    const f32 bottom = walk.world.scene().worldTransform(kD3Stone)[3].y;
    raiseD3Stone(walk);
    const LevelTriggers& triggers = walk.world.triggers();
    bool withinStep = false;
    for (s32 frame = 0; frame < 5 * hz && !triggers.settled(kD3Lift); ++frame) {
        walk.step(kTowardStone);
        CHECK(walk.party[0].floor.object != kD3Stone);
        // While the stone still rises within a step down of the walkway, every other probe
        // meets it and the player's passes through it.
        const Vec3 stone{walk.world.scene().worldTransform(kD3Stone)[3]};
        if (!triggers.settled(kD3Lift) &&
            stone.y >= walk.party[0].actor.floorBase() - PlayerActor::kStepUp) {
            const Vec3 over = stone + Vec3{0, 0.5f, 0};
            const auto other = walk.world.collision().floorAt(over, 1, 1);
            withinStep |= other.has_value() && other->object == kD3Stone;
            const auto player = walk.world.collision().floorAt(
                over, 1, 1, 0, WorldCollision::kPlayerSide, WorldCollision::Prober::Player);
            CHECK_FALSE((player.has_value() && player->object == kD3Stone));
        }
    }
    CHECK(withinStep);
    REQUIRE(triggers.settled(kD3Lift));
    CHECK(walk.world.scene().worldTransform(kD3Stone)[3].y == Approx(bottom + kD3Rise));
    for (s32 frame = 0; frame < hz && walk.party[0].floor.object != kD3Stone; ++frame) {
        walk.step(kTowardStone);
    }
    CHECK(walk.party[0].floor.object == kD3Stone);
}

/** G1's crates under the bridge: G1ELEV7 and G1ELEV6, which the hidden pads #53 and #54 on the
 * stump G1ELEV5 raise ten and twenty units out of the water, each carrying a deck hung from it,
 * G1CRATE2 beside the stump and G1CRATE1 beyond it under the Super Shot, XBOWICON #398. */
constexpr s32 kG1Stump = 229;
constexpr s32 kG1NearLift = 820;
constexpr s32 kG1NearCrate = 821;
constexpr s32 kG1FarLift = 818;
constexpr s32 kG1FarCrate = 819;
constexpr s32 kG1SuperShot = 398;
constexpr Vec3 kG1Pads{54.69f, 17.7f, -203.05f}; ///< on the stump, within both hidden pads
constexpr std::array kG1Lifts{kG1NearLift, kG1FarLift};
constexpr std::array kG1Crates{kG1NearCrate, kG1FarCrate};
constexpr f32 kG1Arrival = 0.5f; ///< as near the Super Shot as a walk's waypoint
constexpr s32 kG1RunSeconds = 10;

TEST_CASE("G1 crates under the bridge reach their tops before a player running at them boards",
          "[native-traversal][g1-rising-crates][assets]") {
    // Rising, each lift and the deck hung from it are open to the players' own probes
    // (ProcessItemWobjs marks WorldObj +0x35 for a kind without 8, 0x80062F40-0x80062F4C, and
    // WorldCollide 0x8000D578 takes the direct parent's mark with the object's own), and a lift
    // holds only for a player whose floor is the lift itself (did_generate 0x80063854, the height
    // hold at 0x80062E40-0x80062E68), which a deck's rider never is. A player running from the
    // pads at the crates waits at the stump's edge until each has stopped at its top, crosses
    // both and takes the Super Shot, never sinking into the water. The decks rock on their own
    // keyed loop, so their lifts give the heights.
    const s32 hz = GENERATE(30, 60);
    const s32 character = GENERATE(0, kArcherClass);
    CAPTURE(hz, character);
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(character);
    REQUIRE(stats != nullptr);
    NativeWalk walk("G1", kG1Pads, 1.0f / static_cast<f32>(hz));
    const auto& objects = walk.world.layout().objects();
    REQUIRE(objects[kG1NearCrate].name == "G1CRATE2");
    REQUIRE(objects[kG1FarCrate].name == "G1CRATE1");
    walk.world.setPlayerCount(1);
    walk.stand(kG1Pads, character, *stats);
    REQUIRE(walk.party[0].floor.object == kG1Stump);
    usize superShot = walk.world.placedItems().size();
    for (usize i = 0; i < walk.world.placedItems().size(); ++i) {
        if (walk.world.placedItems().item(i).instance == kG1SuperShot) {
            superShot = i;
        }
    }
    REQUIRE(superShot < walk.world.placedItems().size());
    const auto& prize = walk.world.placedItems().item(superShot);
    REQUIRE(prize.name == "XBOWICON");
    const Vec2 goal{prize.position.x, prize.position.z};
    const LevelTriggers& triggers = walk.world.triggers();
    walk.step(); // on the pads: both lifts set off
    REQUIRE(triggers.opened(kG1NearLift));
    REQUIRE(triggers.opened(kG1FarLift));
    REQUIRE_FALSE(triggers.settled(kG1NearLift));
    REQUIRE_FALSE(triggers.settled(kG1FarLift));
    std::array<bool, kG1Crates.size()> withinStep{};
    const f32 start = walk.party[0].actor.position().y;
    f32 lowest = start;
    bool crossedNear = false;
    bool took = false;
    bool arrived = false;
    const Chests chests;
    for (s32 frame = 0; frame < kG1RunSeconds * hz && !arrived; ++frame) {
        const Vec3 at = walk.party[0].actor.position();
        const Vec2 toward = goal - Vec2{at.x, at.z};
        walk.step(glm::length(toward) > 0 ? glm::normalize(toward) : Vec2{0});
        const PlayerActor& actor = walk.party[0].actor;
        const s32 floor = walk.party[0].floor.object;
        lowest = std::min(lowest, actor.position().y);
        crossedNear |= floor == kG1NearCrate;
        for (usize i = 0; i < kG1Crates.size(); ++i) {
            if (triggers.settled(kG1Lifts[i])) {
                continue;
            }
            CHECK(floor != kG1Crates[i]);
            // While the deck still rises within a step below the player, every other probe
            // meets it and the player's passes through it.
            const Vec3 deck{walk.world.scene().worldTransform(static_cast<usize>(kG1Crates[i]))[3]};
            const Vec3 over{deck.x, actor.floorBase(), deck.z};
            const auto other = walk.world.collision().floorAt(over, FloorRiding::kProbeAbove,
                                                              PlayerActor::kStepUp);
            if (other && other->object == kG1Crates[i]) {
                withinStep[i] = true;
                const auto player = walk.world.collision().floorAt(
                    over, FloorRiding::kProbeAbove, PlayerActor::kStepUp, 0,
                    WorldCollision::kPlayerSide, WorldCollision::Prober::Player);
                CHECK_FALSE((player.has_value() && player->object == kG1Crates[i]));
            }
        }
        const auto collectors = PartyPickups::collectors(walk.party, walk.world, chests);
        for (const Pickup& pickup : walk.world.placedItems().collect(walk.device, collectors)) {
            took |= pickup.item == superShot;
        }
        if (took) {
            CHECK(triggers.settled(kG1FarLift)); // never from a deck still under the water
        }
        const Vec3 now = actor.position();
        arrived =
            took && floor == kG1FarCrate && glm::length(goal - Vec2{now.x, now.z}) < kG1Arrival;
    }
    CHECK(withinStep[0]);
    CHECK(withinStep[1]);
    for (const s32 lift : kG1Lifts) {
        CAPTURE(lift);
        CHECK(triggers.settled(lift));
        const auto object = static_cast<usize>(lift);
        CHECK(walk.world.scene().worldTransform(object)[3].y == Approx(objects[object].position.y));
    }
    CHECK(crossedNear);
    CHECK(took);
    CHECK(arrived);
    CHECK(lowest > start - PlayerActor::kStepUp);
}

/** J4's flipping tile: the lever TRIGGER3 #428 slides J4A35 (601) twenty units west, bringing the
 * spiked tile J4FLIPPPER (602) to x -164..-154. Stood on, the tile's pad #356 swings the rainbow
 * tile in beside it on J4A38, bringing the whole-party pad #359 within reach, which rolls J4A36
 * (600) a half turn about its hinge at x -154 and, through #357, J4A37 (599) a half turn about
 * x -144: the tile leapfrogs under the rainbow tile and comes up on its far side, x -144..-134. */
constexpr s32 kJ4Lever = 428;
constexpr s32 kJ4Slide = 601;
constexpr s32 kJ4Tile = 602;
constexpr s32 kJ4FirstRoll = 600;
constexpr s32 kJ4SecondRoll = 599;
constexpr std::array kJ4Rolls{kJ4FirstRoll, kJ4SecondRoll};
constexpr Vec3 kJ4Rider{-155.5f, 26, -26.75f}; ///< on the slid tile, a unit and a half short of
                                               ///< its hinge, within #359's reach
constexpr f32 kJ4FarWest = -144;
constexpr f32 kJ4FarEast = -134;
constexpr s32 kJ4WaitSeconds = 10;

TEST_CASE("J4's flipping tile carries the player standing on it over to the far side",
          "[native-traversal][j4-flipping-tile][assets]") {
    // PlayerCheckFloor (0x80087F88) parents a player to a floor flagged 0x1000, and
    // PlayerCollideFloor (0x80088068) zeroes the whole step, leaving the floor as it is, while the
    // flags of the floor and its parents (WorldObjGetAllFlags, 0x800AB47C) hold 0x0C000000 and
    // 0x20000000 and the probe finds another floor or none (0x80088128-0x80088174). DoWorldAnimSub
    // sets 0x08000000 on every pass a track plays (0x800A8A54-0x800A8A68), and ProcessItemWobjs'
    // tail 0x20000000 on every target that moved, whatever its kind (0x80062F38-0x80062F64): the
    // rolling tile keeps its rider for both half turns, whoever stands in #359.
    const s32 hz = GENERATE(30, 60);
    CAPTURE(hz);
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(kKnightClass);
    REQUIRE(stats != nullptr);
    NativeWalk walk("J4", kJ4Rider, 1.0f / static_cast<f32>(hz));
    const auto& objects = walk.world.layout().objects();
    REQUIRE(objects[kJ4Tile].name == "J4FLIPPPER");
    REQUIRE(objects[kJ4FirstRoll].name == "J4A36");
    REQUIRE(objects[kJ4SecondRoll].name == "J4A37");
    const LevelTriggers& triggers = walk.world.triggers();
    const usize leverPad = triggerOf(triggers, kJ4Slide);
    REQUIRE(leverPad < triggers.size());
    REQUIRE(triggers.trigger(leverPad).instance == kJ4Lever);
    walk.world.setPlayerCount(1);
    // At the lever: the tile slides over beside the rainbow tile's place.
    walk.stand(triggers.trigger(leverPad).spot, kKnightClass, *stats);
    for (s32 frame = 0;
         frame < 3 * hz && !(triggers.opened(kJ4Slide) && triggers.settled(kJ4Slide)); ++frame) {
        walk.step();
    }
    REQUIRE(triggers.opened(kJ4Slide));
    REQUIRE(triggers.settled(kJ4Slide));
    walk.stand(kJ4Rider, kKnightClass, *stats);
    REQUIRE(walk.party[0].floor.object == kJ4Tile);
    const f32 startY = walk.party[0].actor.position().y;
    f32 lowest = startY;
    bool rolled = false;
    const auto rolling = [&] {
        return std::ranges::any_of(
            kJ4Rolls, [&](s32 roll) { return triggers.opened(roll) && !triggers.settled(roll); });
    };
    const auto rolledOver = [&] {
        return std::ranges::all_of(
            kJ4Rolls, [&](s32 roll) { return triggers.opened(roll) && triggers.settled(roll); });
    };
    for (s32 frame = 0; frame < kJ4WaitSeconds * hz && !rolledOver(); ++frame) {
        walk.step();
        rolled |= rolling();
        const Vec3 at = walk.party[0].actor.position();
        CAPTURE(frame, at.x, at.y, at.z);
        lowest = std::min(lowest, at.y);
        REQUIRE(walk.party[0].floor.object == kJ4Tile);
    }
    CHECK(rolled);
    REQUIRE(rolledOver());
    const Vec3 end = walk.party[0].actor.position();
    CAPTURE(end.x, end.y, end.z, lowest);
    CHECK(walk.party[0].floor.object == kJ4Tile);
    CHECK(end.x > kJ4FarWest);
    CHECK(end.x < kJ4FarEast);
    // Left alone on the far side, it stands on the tile's top, as high as it started.
    walk.wait(hz);
    const Vec3 rest = walk.party[0].actor.position();
    CAPTURE(rest.x, rest.y, rest.z);
    CHECK(walk.party[0].floor.object == kJ4Tile);
    CHECK(rest.y == Approx(startY).margin(0.01f));
    const auto under = walk.world.collision().floorAt(rest, 1, 1);
    REQUIRE(under.has_value());
    CHECK(under->object == kJ4Tile);
    CHECK(under->y == Approx(rest.y).margin(0.01f));
}

} // namespace
