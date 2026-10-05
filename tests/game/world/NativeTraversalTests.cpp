#include <array>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerActor.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PlayerRuntime.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelTriggers.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr f32 kStep = 1.0f / 30.0f;

struct NativeWalk {
    test::FakeRenderDevice device;
    LevelWorld world;
    std::array<PlayerRuntime, 1> party;
    Vec3 carried{0};
    Vec3 travelled{0};
    Vec3 landed{0};
    s32 ownershipDiagnostics = 0;

    NativeWalk(std::string_view level, Vec3 start) {
        const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
        LevelCatalog catalog;
        REQUIRE(catalog.load(root));
        const auto info = catalog.byName(level);
        REQUIRE(info.has_value());
        REQUIRE(world.load(device, root, *info));
        world.startTriggers({});
        world.updateTriggers(0, {});
        party[0].actor.spawn(0, {}, nullptr, start, 0);
        party[0].actor.settle(world.collision());
        FloorRiding::land(party, 0, party[0].actor.position(), world.collision());
    }

    void step(Vec2 direction = Vec2{0}) {
        auto& actor = party[0].actor;
        world.update(kStep);
        FloorRiding::carry(party[0], world.collision());
        carried = actor.position();
        const Vec3 before = actor.position();
        actor.clearWallContacts();
        actor.update(MoveInput{.direction = direction,
                               .magnitude = glm::length(direction) > 0 ? 1.0f : 0.0f},
                     0, kStep, &world.collision());
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
        actor.fall(kStep, world.collision());
        const std::array visitors{TriggerVisitor{.position = actor.position(),
                                                 .radius = actor.radius(),
                                                 .floorObject = party[0].floor.object,
                                                 .height = actor.height()}};
        world.updateTriggers(kStep, visitors);
    }

    void walk(Vec2 goal) {
        const auto& actor = party[0].actor;
        for (s32 frame = 0; frame < 240; ++frame) {
            const Vec2 delta = goal - Vec2{actor.position().x, actor.position().z};
            if (glm::length(delta) < 0.3f) {
                return;
            }
            step(glm::normalize(delta));
        }
        CAPTURE(goal.x, goal.y, actor.position().x, actor.position().y, actor.position().z,
                party[0].floor.object);
        const Vec3 delta{goal.x - actor.position().x, 0, goal.y - actor.position().z};
        const Vec3 target = actor.position() + glm::normalize(delta) * (actor.speed() * kStep);
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

TEST_CASE("G1 first lift can be boarded from its lower landing and left above",
          "[native-traversal][g1-first-lift][assets]") {
    const f32 offset = GENERATE(-2.0f, 0.0f, 2.0f);
    CAPTURE(offset);
    NativeWalk walk("G1", {41, 10, -85 + offset});
    walk.walk({48.4f, -85 + offset});
    REQUIRE(walk.party[0].floor.object == 1083);
    walk.wait(90);
    REQUIRE(walk.party[0].actor.position().y > 19);
    walk.walk({55, -85 + offset});
    CHECK(walk.party[0].floor.object != 1083);
}

TEST_CASE("G1 first lift riders can leave a pressed corner",
          "[native-traversal][g1-lift-corners][assets]") {
    const f32 side = GENERATE(-1.0f, 1.0f);
    const f32 end = GENERATE(-1.0f, 1.0f);
    const s32 delay = GENERATE(0, 75);
    CAPTURE(side, end, delay);
    NativeWalk walk("G1", {41, 10, -85});
    walk.walk({48.4f, -85});
    walk.wait(delay);
    for (s32 frame = 0; frame < 90; ++frame) {
        walk.step(glm::normalize(Vec2{side, end}));
    }
    walk.walk({48.4f, -85});
    CHECK(walk.party[0].floor.object == 1083);
}

TEST_CASE("G1 second lift can descend and recover from its broken fence corner",
          "[native-traversal][g1-second-lift][assets]") {
    const f32 end = GENERATE(-1.0f, 1.0f);
    CAPTURE(end);
    NativeWalk walk("G1", {-24.375f, 27.3f, -156.71875f});
    walk.wait(90);
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
    for (s32 frame = 0; frame < 90; ++frame) {
        walk.step(glm::normalize(Vec2{-1, end}));
        if (end > 0 && frame >= 30 && frame <= 42) {
            UNSCOPED_INFO("entry frame " << frame << " carry(" << walk.carried.x << ","
                                         << walk.carried.y << "," << walk.carried.z << ") walk("
                                         << walk.travelled.x << "," << walk.travelled.y << ","
                                         << walk.travelled.z << ") land(" << walk.landed.x << ","
                                         << walk.landed.y << "," << walk.landed.z << ") fall("
                                         << walk.party[0].actor.position().x << ","
                                         << walk.party[0].actor.position().y << ","
                                         << walk.party[0].actor.position().z << ") floor "
                                         << walk.party[0].floor.object << " blocked "
                                         << walk.world.collision().floorExitBlocked(273));
        }
    }
    // The north-left strut and fence form a narrowing wedge toward -Z, not a
    // shortcut onto the deck. Their normals allow backing out toward +Z first;
    // native fn_80088714 also stops a move against opposing second-wall normals.
    // Round the north end before boarding through the open middle of that edge.
    if (end > 0) {
        walk.walk({walk.party[0].actor.position().x, -145});
        walk.walk({-17.75f, -145});
    } else {
        walk.walk({walk.party[0].actor.position().x, -152});
    }
    walk.walk({-17.75f, -152});
    walk.walk({-11, -152});
    CHECK(walk.party[0].floor.object != 273);
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

} // namespace
