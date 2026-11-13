#include <array>
#include <functional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"
#include "game/rollback/LocalOwnerContract.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 30.0f;

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    NativeStateResources resources;

    explicit Fixture(std::string_view tag) {
        const auto root = test::sampleLevel(tag);
        std::filesystem::create_directories(root / "wdata");
        writeTextFile(root / "wdata/TEST.json", R"({"levels":[{"name":"TEST","bossType":42}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":2}],
          "trees":[
           {"name":"COIN","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GEMORANGE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GETGEMORANGE","nodes":[{"name":"SPARK","particle":0,"parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"BURST","frames":6,"frameRate":30}]}]})");
        writeTextFile(root / "world.json", R"({
          "objects":[
            {"name":"FLOOR","position":[0,0,0],"child":1,"next":2,"flags":4100},
            {"name":"WALL","position":[0,3,0],"next":-1},
            {"name":"WINDOW","position":[20,0,0],"next":3},
            {"name":"WALL","position":[40,0,0],"next":4,"flags":327680},
            {"name":"WALL","position":[60,0,0],"next":5,"flags":4096},
            {"name":"L1PSYSE_BRAZIER","position":[70,1,0],"next":-1,"flags":2048}],
          "bounds":{"min":[-20,-10,-20],"max":[100,20,20]},
          "animations":[
            {"object":0,"frames":4,"state":257,"start":0,
             "track":{"flags":32,"frames":[0,3],"values":[0,4]}},
            {"object":3,"frames":4,"state":257,"start":0,
             "track":{"flags":16,"frames":[0,3],"values":[0,2]}}],
          "particles":[{"id":"E","preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "itemInfos":[
            {"type":5,"subtype":24,"name":"PAD","radius":2,"height":1},
            {"type":12,"subtype":0,"name":"SPIN","radius":2,"height":1},
            {"type":10,"subtype":40,"name":"WALL","radius":2,"height":3},
            {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":1,"radius":2,"height":5},
            {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2},
            {"type":10,"subtype":51,"name":"WALL","radius":2,"height":3}],
          "itemInstances":[
            {"info":0,"flags":2,"position":[0,0,0],"params":[0,0,2,0,4,255,1,0,0,0,0,0]},
            {"info":0,"flags":2,"position":[20,0,0],"params":[2,0,16,0,4,255,2,0,0,0,0,0]},
            {"info":1,"flags":2,"position":[60,0,0],"params":[4,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":2,"name":"WALL","position":[30,0,0]},
            {"info":3,"name":"WALL","position":[50,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
            {"info":4,"position":[0,0,0]},
            {"info":5,"position":[80,0,0]},
            {"info":6,"name":"WALL","position":[90,0,0]}]})");
        writeTextFile(root / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0],"vertices":[-3,0,-3,3,0,-3,0,0,3]}]})");
        REQUIRE(world.load(device, root, LevelRef{"TEST", 0, "TEST", "", "", "", "", 0}));
        world.setPlayerCount(1);
        REQUIRE(world.worldAnimator().size() == 2);
        REQUIRE(world.rotators().size() == 1);
        REQUIRE(world.fallingScenery().size() == 1);
        REQUIRE(world.skorneArena().size() == 1);
        REQUIRE(world.walls().size() == 1);
        REQUIRE(world.placedItems().size() == 2);
        REQUIRE(world.particles().size() == 1);
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(resources.registerLayout(world.layout(), "TEST"));
        REQUIRE(resources.registerCollision(world.collision(), "TEST"));
        REQUIRE(resources.registerTextures(world.textures(), "TEST/textures"));
        REQUIRE(resources.registerArchive(world.items(), "TEST/items"));
        if (world.powerups().loaded()) {
            REQUIRE(resources.registerArchive(world.powerups(), "POWERUPS"));
        }
        REQUIRE(world.registerNativeResources(resources));
    }

    NativeStateDigest::Digest hash() {
        const auto uploads = device.texturesCreated;
        const auto updates = device.textureUpdates;
        const auto draws = device.draws.size();
        REQUIRE(world.registerNativeResources(resources));
        NativeStateDigest digest(resources.resources());
        REQUIRE(world.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        CHECK(device.texturesCreated == uploads);
        CHECK(device.textureUpdates == updates);
        CHECK(device.draws.size() == draws);
        return *digest.checkedFinish();
    }

    NativeStateDigest::Digest hash(const LevelWorld::LocalSnapshot& saved) {
        REQUIRE(world.registerNativeResources(resources));
        NativeStateDigest digest(resources.resources());
        REQUIRE(world.appendLocalDigest(digest, saved));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    }

    /** The level's resources, refreshed as hash() refreshes them, for one of its owners. */
    NativeStateDigest::Resources refreshed() {
        REQUIRE(world.registerNativeResources(resources));
        return resources.resources();
    }

    void step(s32 tick) {
        world.capturePresentation();
        if (tick == 1) {
            REQUIRE_FALSE(world.fallingScenery().touch({30, 0, 0}, 1).empty());
            REQUIRE(world.throwItem(device, "COIN", {8, 5, 0}, {2, 3, 0}, 0.5f));
            world.bossArenaCue({0, 0, 0});
        }
        if (tick == 3) {
            REQUIRE(world.strikeWall(0, 10));
        }
        if (tick == 5) {
            REQUIRE(world.collect(device, std::array{Collector{{80, 0, 0}, 1, 2}}).size() == 1);
            world.bossArenaCue({0, 0, 0});
        }
        if (tick == 7) {
            world.bossArenaCue({1, 0, 2});
        }
        world.update(kStep, tick >= 9 && tick <= 11);
        const std::array visitors{TriggerVisitor{.position = Vec3{0, 0, 0}, .floorObject = 0},
                                  TriggerVisitor{.position = Vec3{20, 0, 0}}};
        world.updateTriggers(kStep, visitors);
        world.setAmbientOffset(-static_cast<f32>(tick % 6) / 10);
        const std::array<PointLight, 1> lights{{{{10, 2, 0}, {1, 0.25f, 0.5f}, 8, 2}}};
        world.setPointLights(lights);
    }

    void draw(f32 alpha) {
        device.draws.clear();
        world.draw(device, Mat4{1}, WorldCamera{}, alpha);
        REQUIRE_FALSE(device.draws.empty());
    }
};

TEST_CASE("native world digest agrees across independent bindings and render cadences",
          "[native-digest][level-world-native-digest]") {
    Fixture left("native-world-left");
    Fixture right("native-world-right");
    REQUIRE(&left.world.items() != &right.world.items());
    REQUIRE(left.world.placedItems().item(0).model != right.world.placedItems().item(0).model);
    CHECK(left.hash() == right.hash());
    for (s32 tick = 1; tick <= 24; ++tick) {
        CAPTURE(tick);
        left.step(tick);
        right.step(tick);
        const auto before = left.hash();
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            left.draw(alpha);
            CHECK(left.hash() == before);
        }
        if (tick % 3 == 0) {
            right.draw(0.8f);
        }
        CHECK(right.hash() == before);
        LevelWorld::LocalSnapshot saved;
        left.world.saveLocal(saved);
        CHECK(left.hash(saved) == before);
    }
}

TEST_CASE("native world saved hash survives wrong branches and one to eight tick replay",
          "[native-digest][level-world-native-digest]") {
    Fixture replay("native-world-replay");
    Fixture reference("native-world-reference");
    for (s32 tick = 1; tick <= 2; ++tick) {
        replay.step(tick);
        reference.step(tick);
    }
    LevelWorld::LocalSnapshot anchor;
    replay.world.saveLocal(anchor);
    const auto anchorHash = replay.hash(anchor);
    std::vector<NativeStateDigest::Digest> expected;
    for (s32 tick = 3; tick <= 10; ++tick) {
        reference.step(tick);
        expected.push_back(reference.hash());
    }
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(replay.world.restoreLocal(anchor));
        replay.world.setPlayerCount(4);
        replay.world.bossArenaCue({-99, 0, 27});
        replay.world.bossArenaCue({23, 0, -18});
        replay.world.discardItem(0);
        replay.world.revealWall(0);
        for (s32 tick = 0; tick < distance; ++tick) {
            replay.world.update(kStep);
        }
        CHECK(replay.hash(anchor) == anchorHash);
        CHECK(replay.hash() != anchorHash);
        REQUIRE(replay.world.restoreLocal(anchor));
        CHECK(replay.hash() == anchorHash);
        for (s32 tick = 3; tick < 3 + distance; ++tick) {
            replay.step(tick);
        }
        CHECK(replay.hash() == expected[static_cast<usize>(distance - 1)]);
    }
}

TEST_CASE("native world hash retains pickup hidden continuation fields",
          "[native-digest][level-world-native-digest]") {
    Fixture fixture("native-world-hidden-item");
    fixture.step(1);
    LevelWorld::LocalSnapshot saved;
    fixture.world.saveLocal(saved);
    const auto original = fixture.hash();
    using Item = PlacedItems::Item;
    const std::vector<std::function<void(Item&)>> changes{
        [](Item& v) { v.noGrabSeconds += 0.03f; },
        [](Item& v) { v.velocity.x += 0.5f; },
        [](Item& v) { v.previousTransform[3].z += 0.5f; },
        [](Item& v) { v.previousFrame += 0.25f; },
        [](Item& v) { ++v.previousGeneration; },
        [](Item& v) { v.previousAlpha -= 0.1f; },
        [](Item& v) { v.presentationCaptured = !v.presentationCaptured; },
        [](Item& v) { ++v.continuity; },
        [](Item& v) { v.contained = !v.contained; },
        [](Item& v) { v.carried = !v.carried; },
        [](Item& v) { ++v.opener; },
        [](Item& v) { v.floor = Item::Floor{0, glm::translate(Mat4{1}, Vec3{1, 2, 3})}; },
        [](Item& v) { v.health += 1; },
        [](Item& v) { v.player.advance(0.01f, true); },
    };
    for (usize i = 0; i < changes.size(); ++i) {
        CAPTURE(i);
        REQUIRE(fixture.world.restoreLocal(saved));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        changes[i](const_cast<Item&>(fixture.world.placedItems().item(0)));
        CHECK(fixture.hash() != original);
        CHECK(fixture.hash(saved) == original);
    }
}

TEST_CASE("native world resource and history failures cannot certify a partial digest",
          "[native-digest][level-world-native-digest]") {
    Fixture fixture("native-world-reject");
    const Fixture foreign("native-world-foreign");
    LevelWorld::LocalSnapshot saved;
    fixture.world.saveLocal(saved);
    NativeStateDigest unbound;
    CHECK_FALSE(fixture.world.appendNativeDigest(unbound));
    CHECK_FALSE(unbound.checkedFinish());
    NativeStateDigest wrong(foreign.resources.resources());
    CHECK_FALSE(foreign.world.appendLocalDigest(wrong, saved));
    CHECK_FALSE(wrong.checkedFinish());
    fixture.world.clear();
    NativeStateDigest stale(fixture.resources.resources());
    CHECK_FALSE(fixture.world.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("native world digest includes undelivered destruction events",
          "[native-digest][level-world-native-digest]") {
    Fixture fixture("native-world-pending-events");
    REQUIRE(fixture.world.explodeObject(3, {40, 0, 0}));
    REQUIRE(fixture.world.hasWorldExplosions());
    LevelWorld::LocalSnapshot queued;
    fixture.world.saveLocal(queued);
    const auto before = fixture.hash();
    REQUIRE(fixture.world.takeWorldExplosions().size() == 1);
    CHECK_FALSE(fixture.world.hasWorldExplosions());
    CHECK(fixture.hash() != before);
    CHECK(fixture.hash(queued) == before);
    REQUIRE(fixture.world.restoreLocal(queued));
    CHECK(fixture.hash() == before);
    CHECK(fixture.world.takeWorldExplosions() == std::vector<Vec3>{{40, 0, 0}});
}

TEST_CASE("native collision digest covers all masks and captured moving transforms",
          "[native-digest][level-world-native-digest]") {
    WorldCollision collision;
    CollisionTriangle floor;
    floor.object = 0;
    floor.vertices = {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}};
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    const auto hash = [&] {
        NativeStateDigest digest;
        REQUIRE(collision.appendNativeDigest(digest));
        return digest.finish();
    };
    WorldCollision::LocalSnapshot saved;
    collision.saveLocal(saved);
    const auto original = hash();
    for (s32 change = 0; change < 4; ++change) {
        CAPTURE(change);
        REQUIRE(collision.restoreLocal(saved));
        if (change == 0) {
            collision.setSolid(0, false);
        }
        if (change == 1) {
            collision.setContactOnly(0, true);
        }
        if (change == 2) {
            collision.setFloorExitBlocked(0, true);
        }
        if (change == 3) {
            collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}));
        }
        CHECK(hash() != original);
        NativeStateDigest past;
        REQUIRE(collision.appendLocalDigest(past, saved));
        CHECK(past.finish() == original);
    }
}

TEST_CASE("each native world leaf rejects foreign and expired local history",
          "[native-digest][level-world-native-digest]") {
    test::checkNativeOwnerGuards<WorldScene>();
    test::checkNativeOwnerGuards<WorldAnimator>();
    test::checkNativeOwnerGuards<WorldCollision>();
    test::checkNativeOwnerGuards<PlacedItems>();
    test::checkNativeOwnerGuards<SkorneArena>();
    test::checkNativeOwnerGuards<FallingScenery>();
    test::checkNativeOwnerGuards<DestructibleWalls>();
    test::checkNativeOwnerGuards<WorldDestruction>();
    test::checkNativeOwnerGuards<Rotators>();
}

TEST_CASE("falling-piece digest retains previous motion not only its current public pose",
          "[native-digest][level-world-native-digest]") {
    FallingPiece left;
    FallingPiece right;
    left.place({1, 2, 3}, {0.1f, 0.2f, 0.3f}, 7);
    right.place({9, 8, 7}, {0.9f, 0.8f, 0.7f}, 7);
    right.position = left.position;
    right.rotation = left.rotation;
    right.velocity = left.velocity;
    right.visible = left.visible;
    NativeStateDigest a;
    NativeStateDigest b;
    a.value(left);
    b.value(right);
    REQUIRE(a.checkedFinish());
    REQUIRE(b.checkedFinish());
    CHECK(a.finish() != b.finish());
    CHECK(left.presentedPosition(0) != right.presentedPosition(0));
    right = left;
    NativeStateDigest copied;
    copied.value(right);
    CHECK(copied.finish() == a.finish());
}

TEST_CASE("native world digest bytes stay pinned for a bound level and queued cues",
          "[native-digest][level-world-native-digest][native-digest-pin]") {
    Fixture fixture("native-world-pin");
    fixture.world.setPlayerCount(3);
    fixture.world.bossArenaCue({23, 0, -18});
    REQUIRE(fixture.world.explodeObject(3, {40, 0, 0}));
    // SHA-256 of the native digest of the world above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "4fe80eb6b1d9f752dbf1f78a21827955feee6f6c3400df4d7c519cfbeeea2442";
    CHECK(test::hexBytes(fixture.hash()) == kDigest);
}

TEST_CASE("native world and child digest bytes stay pinned after the script and what it saved",
          "[native-digest][level-world-native-digest][digest-golden]") {
    Fixture fixture("native-world-golden");
    fixture.step(1);
    fixture.step(2);
    const LevelWorld& world = fixture.world;
    LevelWorld::LocalSnapshot saved;
    WorldScene::LocalSnapshot scene;
    WorldAnimator::LocalSnapshot animator;
    WorldCollision::LocalSnapshot collision;
    PlacedItems::LocalSnapshot items;
    SkorneArena::LocalSnapshot arena;
    FallingScenery::LocalSnapshot falling;
    DestructibleWalls::LocalSnapshot walls;
    LevelTriggers::LocalSnapshot triggers;
    Rotators::LocalSnapshot rotators;
    world.saveLocal(saved);
    world.scene().saveLocal(scene);
    world.worldAnimator().saveLocal(animator);
    world.collision().saveLocal(collision);
    world.placedItems().saveLocal(items);
    world.skorneArena().saveLocal(arena);
    world.fallingScenery().saveLocal(falling);
    world.walls().saveLocal(walls);
    world.triggers().saveLocal(triggers);
    world.rotators().saveLocal(rotators);
    for (s32 tick = 3; tick <= 7; ++tick) {
        fixture.step(tick);
    }
    // SHA-256 of each owner's native digest seven ticks in and of what it saved two ticks in,
    // which netplay peers must hash alike: a literal moves only when what is visited changes on
    // purpose. The world's children follow it, so that a moved world names the child that moved.
    test::checkPinnedDigests("LevelWorld", fixture.hash(), fixture.hash(saved),
                             "1cebc9fff6c821a46cc2e432e667d439b97bf722d406741588b753e6aa1f5cf2",
                             "c9497b513a9fc5a075e4e27b0cfa96dd1bf9b40ccb235600abe739be242537f9");
    const auto resources = fixture.refreshed();
    test::checkPinnedDigests("WorldScene", test::liveDigest(world.scene(), resources),
                             test::savedDigest(world.scene(), scene, resources),
                             "d35f6d83518347997a8bd8158162061c6da1dda1c2fb782f76d237fd3056153f",
                             "5f13c2c8e64fc8e9f16199c2c9159fd8f9e0568807e68027c7bcb33ea091f52b");
    test::checkPinnedDigests("WorldAnimator", test::liveDigest(world.worldAnimator(), resources),
                             test::savedDigest(world.worldAnimator(), animator, resources),
                             "e2bf498d79074ad0ccb576f75b8faca56de4ad2a28654054338636f147c31306",
                             "9658ac35a748711c349c034caa46b73fc9ee428b1ed7a2dc1123407c3a9cadb9");
    test::checkPinnedDigests("WorldCollision", test::liveDigest(world.collision(), resources),
                             test::savedDigest(world.collision(), collision, resources),
                             "4d1917b395c6906d6fedf704bfbe82d35b7caa8636959da13bef49e06792e9f5",
                             "4fb92b7d327cd8d4b0dddac8d518f60f72771a7efd8b4a9fb17f48e1b948c9ef");
    test::checkPinnedDigests("PlacedItems", test::liveDigest(world.placedItems(), resources),
                             test::savedDigest(world.placedItems(), items, resources),
                             "6a093a8504480677804b3e98b4c5b7df25265f5500ddd2978a57681839287042",
                             "18de69562f7d491bf9d24ab7cc55ce0a7176f4d0adc29b05e8f4a9fc2e16ff13");
    test::checkPinnedDigests("SkorneArena", test::liveDigest(world.skorneArena(), resources),
                             test::savedDigest(world.skorneArena(), arena, resources),
                             "a61b028b814bd841500e04b91e466f386c3741eb963ca839c40f4699cbce81b6",
                             "b6d88061727bf5cfac1fe0a520b74a90ce4d65bad7cdafe29b59c0854840277e");
    test::checkPinnedDigests("FallingScenery", test::liveDigest(world.fallingScenery(), resources),
                             test::savedDigest(world.fallingScenery(), falling, resources),
                             "3dd4065256f71f194f60d6054bd42caba4ed37ecd0258fbd1e4c3de190354b6e",
                             "4b05e2b3ab4c54d4bf2df250612d848144bc8c61fd551cd7d8b54db771c4a233");
    test::checkPinnedDigests("DestructibleWalls", test::liveDigest(world.walls(), resources),
                             test::savedDigest(world.walls(), walls, resources),
                             "387caf19c6590f4bd95ebbe8bdd24672dae1faa2b81c4977a4924b886a9a39b1",
                             "dbdcf92e6baa0db545e387ef07fdf86fa1d8cc7c42ad71c223b20fa777ff5050");
    test::checkPinnedDigests("LevelTriggers", test::liveDigest(world.triggers(), resources),
                             test::savedDigest(world.triggers(), triggers, resources),
                             "c9f79710c307d7360819941ada8b9047d48352a9481dedeeb7d8d49f5f37aae0",
                             "23319f498000ea4e36440783f04f89b45bc7b42238fc2f2c40bc7b42014506fa");
    test::checkPinnedDigests("Rotators", test::liveDigest(world.rotators(), resources),
                             test::savedDigest(world.rotators(), rotators, resources),
                             "962d17489342291f1bf510040168249893fffa723ba24274e75ad9adf5db686d",
                             "19505fa591c19bb6b96a4856edb07e1528ea463296ee18cdc5f5a6b8da772efb");
}

TEST_CASE("lightning rewind retains the next random wait independently of render cadence",
          "[world-lightning][native-digest][level-world-native-digest][local-rewind]") {
    struct LightningFixture {
        test::FakeRenderDevice device;
        LevelWorld world;
        NativeStateResources resources;

        explicit LightningFixture(std::string_view tag) {
            const auto root = test::sampleLevel(tag);
            writeTextFile(root / "objects.json", R"({"objects":[
              {"index":0,"name":"F2XPNC_LNING1","file":"models/000_WALL.obj"},
              {"index":1,"name":"F2XPNC_LNING2","file":"models/000_WALL.obj"},
              {"index":2,"name":"F2XPNC_LNING3","file":"models/000_WALL.obj"},
              {"index":3,"name":"F2XPNC_LNING4","file":"models/000_WALL.obj"},
              {"index":4,"name":"WALL","file":"models/000_WALL.obj"},
              {"index":5,"name":"BRANCH","file":"models/000_WALL.obj"},
              {"index":6,"name":"BOLT","file":"models/000_WALL.obj"}]})");
            writeTextFile(root / "world.json", R"({"objects":[
              {"name":"F2XPNC_LNING1","position":[-4,0,0],"next":1,"child":5},
              {"name":"F2XPNC_LNING2","position":[-2,0,0],"next":2},
              {"name":"F2XPNC_LNING3","position":[0,0,0],"next":3},
              {"name":"F2XPNC_LNING4","position":[2,0,0],"next":4},
              {"name":"WALL","position":[4,0,0],"next":-1},
              {"name":"BRANCH","position":[0,2,0],"child":6,"next":-1},
              {"name":"BOLT","position":[0,2,0],"next":-1}],
              "bounds":{"min":[-10,-10,-10],"max":[10,10,10]}})");
            writeTextFile(root / "animations.json", R"({})");
            writeTextFile(root / "collision.json", R"({"objects":[]})");
            test::convertModelFixture(root);
            REQUIRE(world.load(device, root, LevelRef{"TEST", 0, "F2", "", "", "", "", 0}));
            test::registerStage(resources, world, device);
        }
        void step(bool stopped = false) {
            world.capturePresentation();
            world.update(1.f / 60, stopped);
        }
        NativeStateDigest::Digest hash() { return test::liveDigest(world, resources); }
        NativeStateDigest::Digest hash(const LevelWorld::LocalSnapshot& saved) {
            return test::savedDigest(world, saved, resources);
        }
        void draw() {
            for (const f32 alpha : {0.f, .3f, 1.f}) {
                world.draw(device, Mat4{1}, {}, alpha);
                usize vertices = 0;
                for (const auto& draw : device.draws) {
                    vertices += draw.vertices.size();
                }
                usize expected = 3; // The unrelated, always-visible wall.
                for (const usize object : {0U, 1U, 2U, 3U, 5U, 6U}) {
                    expected += world.scene().objectVisible(object) ? 3U : 0U;
                }
                CHECK(vertices == expected);
                device.draws.clear();
            }
        }
    };
    LightningFixture reference{"lightning-native-reference"};
    LightningFixture replay{"lightning-native-replay"};
    CHECK(reference.hash() == replay.hash());
    for (const usize object : {0U, 1U, 2U, 3U, 5U, 6U}) {
        CHECK_FALSE(replay.world.scene().objectVisible(object));
    }
    replay.draw();

    LevelWorld::LocalSnapshot before;
    NativeStateDigest::Digest beforeDigest{};
    bool flashed = false;
    for (s32 tick = 0; tick < 901 && !flashed; ++tick) {
        replay.world.saveLocal(before);
        beforeDigest = replay.hash();
        reference.step();
        replay.step();
        flashed = replay.world.scene().objectVisible(0);
    }
    REQUIRE(flashed);
    CHECK(replay.world.scene().objectVisible(5));
    CHECK(replay.world.scene().objectVisible(6));
    replay.draw();
    CHECK(reference.hash() == replay.hash());
    CHECK(replay.hash(before) == beforeDigest);
    const auto onDigest = replay.hash();
    const auto uploads = replay.device.texturesCreated;
    // Run past another random draw on the wrong branch, then rewind before
    // the original edge. Restoring visibility alone must not pass this test.
    for (s32 tick = 0; tick < 1000; ++tick) {
        replay.step(true);
    }
    REQUIRE(replay.world.restoreLocal(before));
    CHECK(replay.hash() == beforeDigest);
    CHECK_FALSE(replay.world.scene().objectVisible(0));
    replay.draw();
    CHECK(replay.hash() == beforeDigest);
    replay.step();
    CHECK(replay.hash() == onDigest);
    bool hidden = false;
    bool flashedAgain = false;
    for (s32 tick = 0; tick < 1000; ++tick) {
        reference.step();
        replay.step();
        hidden |= !replay.world.scene().objectVisible(0);
        flashedAgain |= hidden && replay.world.scene().objectVisible(0);
        if (tick % 71 == 0) {
            const auto beforeDraw = replay.hash();
            replay.draw();
            CHECK(replay.hash() == beforeDraw);
        }
        CHECK(replay.hash() == reference.hash());
        for (const usize object : {0U, 1U, 2U, 3U, 5U, 6U}) {
            CHECK(replay.world.scene().objectVisible(object) ==
                  reference.world.scene().objectVisible(object));
        }
        CHECK(replay.world.scene().objectVisible(5) == replay.world.scene().objectVisible(0));
        CHECK(replay.world.scene().objectVisible(6) == replay.world.scene().objectVisible(0));
        CHECK(replay.world.scene().objectVisible(4));
    }
    CHECK(hidden);
    CHECK(flashedAgain);
    CHECK(replay.device.texturesCreated == uploads);
}

} // namespace
