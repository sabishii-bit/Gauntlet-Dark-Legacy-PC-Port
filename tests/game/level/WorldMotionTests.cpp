#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/WorldMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    std::filesystem::path directory = test::sampleLevel("world-motion");
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    LevelTriggers triggers;
    Rotators rotators;
    explicit Fixture(bool oscillates = false) {
        writeTextFile(directory / "world.json", std::string{R"({
          "objects":[
            {"name":"FLOOR","position":[0,0,0],"child":1,"next":2,"flags":4096},
            {"name":"FLOOR","position":[0,1,0],"next":-1},
            {"name":"FLOOR","position":[20,0,0],"next":3,"flags":4096},
            {"name":"WALL","position":[30,0,0],"next":4,"flags":4096},
            {"name":"FLOOR","position":[40,0,0],"next":5,"flags":4096},
            {"name":"FLOOR","position":[50,0,0],"next":6,"flags":4096},
            {"name":"FLOOR","position":[60,0,0],"next":-1}],
          "animations":[{"object":0,"frames":4,"state":257,"start":0,
            "track":{"flags":16,"frames":[0,3],"values":[0,2]}}],
          "itemInfos":[
            {"type":5,"subtype":24,"name":"PAD","radius":2,"height":1},
            {"type":12,"subtype":0,"name":"SPIN","radius":2,"height":1},
            {"type":12,"subtype":2,"name":"TURN","radius":2,"height":1}],
          "itemInstances":[
            {"info":0,"flags":2,"position":[0,0,0],
             "params":[0,0,2,0,4,255,1,0,0,0,0,0]},
            {"info":0,"flags":2,"position":[20,0,0],
             "params":[2,0,)"} + std::to_string(oscillates ? 32 : 2) +
                                                    R"(,0,4,255,2,0,0,0,20,0]},
            {"info":0,"flags":2,"position":[30,0,0],
             "params":[3,0,16,0,4,255,3,0,0,0,0,0]},
            {"info":1,"flags":2,"position":[40,0,0],
             "params":[4,0,0,0,205,204,204,61,0,0,128,63]},
            {"info":2,"flags":2,"position":[50,0,0],
             "params":[5,0,0,0,10,215,35,60,0,0,128,63]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        const std::array<usize, 1> controlled{6};
        REQUIRE(scene.build(layout, models, textures, device, {}, {}, controlled));
        animator.bind(layout);
        animator.apply(scene);
        std::vector<CollisionTriangle> triangles;
        for (s32 object = 0; object < 7; ++object) {
            CollisionTriangle floor;
            floor.object = object;
            floor.vertices = {Vec3{-2, 0, -2}, Vec3{-2, 0, 2}, Vec3{2, 0, -2}};
            if (object == 6) {
                for (auto& vertex : floor.vertices) {
                    vertex.x += 60;
                }
            }
            triangles.push_back(floor);
        }
        std::vector<s32> parents;
        for (const auto& object : layout.objects()) {
            parents.push_back(object.parent);
        }
        collision.build(std::move(triangles), parents);
        const std::array<s32, 6> moving{0, 1, 2, 3, 4, 5};
        collision.setMovingObjects(moving);
        sync();
        triggers.bind(layout, animator, &collision);
        animator.apply(scene);
        triggers.placeTargets(scene, &collision);
        rotators.bind(layout);
        sync();
    }
    void sync() {
        for (s32 object = 0; object < 6; ++object) {
            collision.setObjectTransform(object, scene.worldTransform(static_cast<usize>(object)));
        }
    }
    void before(f32 seconds, bool stopped) {
        animator.step(seconds, scene, stopped);
        sync();
    }
    void after(f32 seconds, bool occupied = true) {
        std::vector<TriggerVisitor> visitors;
        if (occupied) {
            for (const f32 x : {0.0f, 20.0f, 30.0f, 50.0f}) {
                visitors.push_back(TriggerVisitor{.position = Vec3{x, 0, 0}});
            }
        }
        triggers.update(seconds, visitors, animator, scene, &collision);
        rotators.update(seconds, visitors, scene);
        sync();
    }
    WorldMotion::State state(bool stopped = false) const {
        return WorldMotion::capture(animator, triggers, rotators, collision,
                                    layout.objects().size(), stopped);
    }
};

void sameGeometry(const WorldScene& actual, const WorldScene& expected, usize count) {
    for (usize object = 0; object < count; ++object) {
        CHECK(actual.worldTransform(object) == expected.worldTransform(object));
        CHECK(actual.objectAlpha(object) == expected.objectAlpha(object));
        CHECK(actual.objectVisible(object) == expected.objectVisible(object));
    }
}

TEST_CASE(
    "restoring a moving parent never turns its inherited floor lock into a permanent child lock",
    "[world-motion][rollback][lift-floor-inheritance]") {
    Fixture host;
    host.collision.setFloorExitBlocked(0, true);
    REQUIRE(host.collision.floorExitBlocked(1));
    REQUIRE_FALSE(host.collision.ownFloorExitBlocked(1));
    const auto seed = host.state();
    REQUIRE(seed.valid());
    CHECK((seed.collision[0] & WorldMotion::kFloorExitBlocked) != 0);
    CHECK((seed.collision[1] & WorldMotion::kFloorExitBlocked) == 0);
    auto scene = host.scene;
    auto collision = host.collision;
    WorldMotion replay;
    replay.bind(host.layout);
    REQUIRE(replay.prepare(seed, host.scene.geometry(), scene, collision));
    CHECK(collision.floorExitBlocked(1));
    CHECK_FALSE(collision.ownFloorExitBlocked(1));
    collision.setFloorExitBlocked(0, false);
    CHECK_FALSE(collision.floorExitBlocked(1));
}

TEST_CASE("world replay preserves native before and after motion across endpoints and rewinds",
          "[world-motion][rollback]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60, 1.0f / 120);
    const bool oscillates = GENERATE(false, true);
    const bool stopped = GENERATE(false, true);
    Fixture host(oscillates);
    host.before(seconds, stopped);
    host.after(seconds);
    const auto seed = host.state(stopped);
    REQUIRE(seed.valid());
    auto scene = host.scene;
    auto collision = host.collision;
    WorldMotion replay;
    replay.bind(host.layout);
    REQUIRE(replay.prepare(seed, host.scene.geometry(), scene, collision));
    CHECK(replay.checkpoint(collision) == seed);
    sameGeometry(scene, host.scene, host.layout.objects().size());
    bool sawHeightLock = false;
    bool sawHeightSettled = false;
    bool sawRotatorDone = false;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(seconds, oscillates, stopped, tick);
        // Restore several sub-frame clocks and endpoint states, not merely time zero.
        if (tick % 37 == 0) {
            REQUIRE(replay.prepare(host.state(stopped), host.scene.geometry(), scene, collision));
        }
        host.before(seconds, stopped);
        REQUIRE(replay.beforeMotion(seconds, scene, collision));
        sameGeometry(scene, host.scene, host.layout.objects().size());
        CHECK(replay.checkpoint(collision) == host.state(stopped));
        host.after(seconds);
        REQUIRE(replay.afterMotion(seconds, scene, collision));
        sameGeometry(scene, host.scene, host.layout.objects().size());
        CHECK(replay.checkpoint(collision) == host.state(stopped));
        sawHeightLock |= collision.floorExitBlocked(2);
        sawHeightSettled |= host.triggers.settled(2);
        sawRotatorDone |= host.rotators.rotator(1).done;
        const auto child = collision.objectTransform(1);
        REQUIRE(child);
        CHECK(*child == scene.worldTransform(1));
    }
    CHECK(sawHeightLock);
    CHECK(sawHeightSettled == !oscillates);
    CHECK(sawRotatorDone);
}

TEST_CASE("motion restore retains invisible solid and visible nonsolid collision independently",
          "[world-motion][rollback]") {
    Fixture host;
    REQUIRE(host.scene.setObjectVisible(6, false));
    host.collision.setSolid(6, true);
    host.collision.setContactOnly(6, true);
    host.collision.setFloorExitBlocked(6, true);
    host.scene.setObjectAlpha(3, 1);
    host.collision.setSolid(3, false);
    const auto captured = host.state();
    auto scene = host.scene;
    auto collision = host.collision;
    collision.setSolid(6, false);
    collision.setContactOnly(6, false);
    collision.setFloorExitBlocked(6, false);
    collision.setSolid(3, true);
    WorldMotion replay;
    replay.bind(host.layout);
    REQUIRE(replay.prepare(captured, host.scene.geometry(), scene, collision));
    CHECK_FALSE(scene.objectVisible(6));
    CHECK(collision.solid(6));
    CHECK(collision.contactOnly(6));
    CHECK(collision.floorExitBlocked(6));
    CHECK(scene.objectAlpha(3) == 1);
    CHECK_FALSE(collision.solid(3));
    CHECK(replay.checkpoint(collision) == captured);
}

TEST_CASE("motion restore retains what a moving lift opens to the players' probes",
          "[world-motion][rollback][player-pass-through]") {
    Fixture host;
    host.collision.setPlayerPassThrough(4, true);
    const auto captured = host.state();
    REQUIRE(captured.valid());
    CHECK((captured.collision[4] & WorldMotion::kPlayerPassThrough) != 0);
    CHECK((captured.collision[5] & WorldMotion::kPlayerPassThrough) == 0);
    auto scene = host.scene;
    auto collision = host.collision;
    collision.setPlayerPassThrough(4, false);
    collision.setPlayerPassThrough(5, true);
    WorldMotion replay;
    replay.bind(host.layout);
    REQUIRE(replay.prepare(captured, host.scene.geometry(), scene, collision));
    CHECK(collision.ownPlayerPassThrough(4));
    CHECK_FALSE(collision.ownPlayerPassThrough(5));
    CHECK(collision.solid(4));
    CHECK(replay.checkpoint(collision) == captured);
}

TEST_CASE("world motion rejects malformed and foreign states atomically",
          "[world-motion][rollback]") {
    Fixture host;
    host.before(1.0f / 60, false);
    host.after(1.0f / 60);
    const auto good = host.state();
    auto scene = host.scene;
    auto collision = host.collision;
    WorldMotion replay;
    replay.bind(host.layout);
    REQUIRE(replay.prepare(good, host.scene.geometry(), scene, collision));
    for (s32 variant = 0; variant < 19; ++variant) {
        auto bad = good;
        switch (variant) {
        case 0: bad.collision.clear(); break;
        case 1: bad.collision[0] = static_cast<u8>(WorldMotion::kPlayerPassThrough << 1); break;
        case 2: bad.collision.push_back(1); break;
        case 3: bad.animations[0].frame = std::numeric_limits<f32>::quiet_NaN(); break;
        case 4: ++bad.animations[0].frames; break;
        case 5: bad.animations[0].object = 1; break;
        case 6: bad.animations.push_back(bad.animations.front()); break;
        case 7: bad.triggers.frameRemainder = 1; break;
        case 8: bad.triggers.targets[0].alpha = -1; break;
        case 9: bad.triggers.targets[0].height = 1; break;
        case 10: bad.triggers.targets[1].object = 1; break;
        case 11: bad.triggers.targets.push_back(bad.triggers.targets.front()); break;
        case 12: bad.rotators[0].turned = std::numeric_limits<f32>::infinity(); break;
        case 13: bad.rotators[0].turned = 10; break;
        case 14: bad.rotators[0].started = true; break;
        case 15: ++bad.rotators[0].instance; break;
        case 16: bad.rotators[0].object = 6; break;
        case 17:
            bad.rotators[1].done = true;
            bad.rotators[1].started = false;
            break;
        case 18: bad.rotators.clear(); break;
        default: FAIL("unknown motion mutation");
        }
        CAPTURE(variant);
        CHECK_FALSE(replay.prepare(bad, host.scene.geometry(), scene, collision));
        CHECK(replay.checkpoint(collision) == good);
        sameGeometry(scene, host.scene, host.layout.objects().size());
    }
    auto foreign = host.scene.geometry();
    ++foreign.layout;
    CHECK_FALSE(replay.prepare(good, foreign, scene, collision));
    CHECK(replay.checkpoint(collision) == good);
    for (const f32 seconds : {-1.0f, 0.251f, std::numeric_limits<f32>::quiet_NaN(),
                              std::numeric_limits<f32>::infinity()}) {
        CHECK_FALSE(replay.beforeMotion(seconds, scene, collision));
        CHECK_FALSE(replay.afterMotion(seconds, scene, collision));
        CHECK(replay.checkpoint(collision) == good);
        sameGeometry(scene, host.scene, host.layout.objects().size());
    }
}

TEST_CASE("passive motion does not activate unobserved pads or emit native trigger events",
          "[world-motion][rollback]") {
    Fixture host;
    WorldMotion replay;
    replay.bind(host.layout);
    auto scene = host.scene;
    auto collision = host.collision;
    REQUIRE(replay.prepare(host.state(true), host.scene.geometry(), scene, collision));
    for (s32 tick = 0; tick < 20; ++tick) {
        REQUIRE(replay.beforeMotion(1.0f / 60, scene, collision));
        REQUIRE(replay.afterMotion(1.0f / 60, scene, collision));
    }
    const auto passive = replay.checkpoint(collision);
    CHECK_FALSE(passive.rotators[1].started);
    CHECK(passive.rotators[1].turned == 0);
    CHECK_FALSE(passive.triggers.targets[0].open);
    CHECK(passive.animations[0].held);
    // Rotators keep their native independent clock under Stop Time.
    CHECK(passive.rotators[0].turned != 0);
    host.triggers.activate(2, WorldAnimator::Finish::Animated, host.animator, host.scene,
                           &host.collision);
    host.triggers.takeOpenings();
    for (s32 tick = 0; tick < 120; ++tick) {
        REQUIRE(host.triggers.advanceMotion(1.0f / 60, host.animator, host.scene, &host.collision));
    }
    CHECK(host.triggers.settled(2));
    CHECK(host.triggers.takeOpenings().empty());
    CHECK(host.triggers.takeSettled().empty());
    CHECK(host.triggers.takeRefusals().empty());
    CHECK(host.triggers.takeLessons().empty());
    CHECK(host.triggers.takeCameraCues().empty());
    CHECK(host.triggers.takeWakes().empty());
}

TEST_CASE("world motion rejects over-budget rosters before touching the bound scene",
          "[world-motion][rollback]") {
    const Fixture host;
    const auto good = host.state();
    WorldMotion replay;
    replay.bind(host.layout);
    auto scene = host.scene;
    auto collision = host.collision;
    REQUIRE(replay.prepare(good, host.scene.geometry(), scene, collision));
    for (s32 roster = 0; roster < 4; ++roster) {
        auto bad = good;
        switch (roster) {
        case 0: bad.collision.resize(WorldMotion::kMaxObjects + 1, 1); break;
        case 1: bad.animations.resize(WorldMotion::kMaxAnimations + 1); break;
        case 2: bad.triggers.targets.resize(LevelTriggers::Motion::kMaxTargets + 1); break;
        case 3: bad.rotators.resize(Rotators::kMaxCursors + 1); break;
        default: FAIL("unknown motion roster");
        }
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(replay.prepare(bad, host.scene.geometry(), scene, collision));
        CHECK(replay.checkpoint(collision) == good);
        sameGeometry(scene, host.scene, host.layout.objects().size());
    }
}

TEST_CASE("native moving world captures bind against trusted level rosters",
          "[world-motion][rollback][assets]") {
    const std::string name = GENERATE("G1", "G3", "J4", "I2");
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld host;
    REQUIRE(host.load(device, root, *level));
    host.startTriggers({});
    host.update(1.0f / 60);
    host.updateTriggers(1.0f / 60, {});
    const auto captured = host.motion();
    REQUIRE(captured.valid());
    WorldMotion replay;
    replay.bind(host.layout());
    auto scene = host.scene();
    auto collision = host.collision();
    REQUIRE(replay.prepare(captured, host.scene().geometry(), scene, collision));
    CHECK(replay.checkpoint(collision) == captured);
    for (s32 tick = 0; tick < 12; ++tick) {
        host.update(1.0f / 60);
        REQUIRE(replay.beforeMotion(1.0f / 60, scene, collision));
        host.updateTriggers(1.0f / 60, {});
        REQUIRE(replay.afterMotion(1.0f / 60, scene, collision));
        CHECK(replay.checkpoint(collision) == host.motion());
        sameGeometry(scene, host.scene(), host.layout().objects().size());
    }
}
} // namespace
