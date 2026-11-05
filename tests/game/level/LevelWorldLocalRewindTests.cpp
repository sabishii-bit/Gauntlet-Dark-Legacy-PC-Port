#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldAnimator.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"
#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A world's native digest over its own stage, so two separately loaded worlds compare. */
NativeStateDigest::Digest worldDigest(LevelWorld& world, const RenderDevice& device) {
    NativeStateResources resources;
    test::registerStage(resources, world, device);
    return test::liveDigest(world, resources);
}

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    explicit Fixture(std::string_view tag) {
        const auto root = test::sampleLevel(tag);
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
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2}],
          "itemInstances":[
            {"info":0,"flags":2,"position":[0,0,0],"params":[0,0,2,0,4,255,1,0,0,0,0,0]},
            {"info":0,"flags":2,"position":[20,0,0],"params":[2,0,16,0,4,255,2,0,0,0,0,0]},
            {"info":1,"flags":2,"position":[60,0,0],"params":[4,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":2,"name":"WALL","position":[30,0,0]},
            {"info":3,"name":"WALL","position":[50,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
            {"info":4,"position":[0,0,0]},
            {"info":5,"position":[80,0,0]}]})");
        writeTextFile(root / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0],"vertices":[-3,0,-3,3,0,-3,0,0,3]}]})");
        const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
        REQUIRE(world.load(device, root, level));
        world.setPlayerCount(1);
        REQUIRE(world.worldAnimator().size() == 2);
        REQUIRE(world.rotators().size() == 1);
        REQUIRE(world.fallingScenery().size() == 1);
        REQUIRE(world.walls().size() == 1);
        REQUIRE(world.placedItems().size() == 2);
        REQUIRE(world.particles().size() == 1);
    }
    NativeStateDigest::Digest digest() { return worldDigest(world, device); }
    void step(s32 tick, f32 seconds) {
        world.capturePresentation();
        if (tick == 1) {
            REQUIRE_FALSE(world.fallingScenery().touch({30, 0, 0}, 1).empty());
            REQUIRE(world.throwItem(device, "COIN", {8, 5, 0}, {2, 3, 0}, 0.5f));
        }
        if (tick == 3) {
            REQUIRE(world.strikeWall(0, 10));
        }
        if (tick == 5) {
            REQUIRE(world.collect(device, std::array{Collector{{80, 0, 0}, 1, 2}}).size() == 1);
        }
        if (tick == 14) {
            REQUIRE(world.strikeWall(0, 30) == 0);
        }
        world.update(seconds, tick >= 9 && tick <= 11);
        const std::array visitors{TriggerVisitor{.position = Vec3{0, 0, 0}, .floorObject = 0},
                                  TriggerVisitor{.position = Vec3{20, 0, 0}}};
        world.updateTriggers(seconds, visitors);
        world.setAmbientOffset(-static_cast<f32>(tick % 6) / 10);
        const std::array<PointLight, 1> lights{{{{10, 2, 0}, {1, 0.25f, 0.5f}, 8, 2}}};
        world.setPointLights(lights);
    }
};

TEST_CASE("native whole world history restores coupled platforms items destruction and triggers",
          "[local-rewind][level-world-local-rewind]") {
    Fixture native("world-local-native");
    Fixture replay("world-local-replay");
    std::array<LevelWorld::LocalSnapshot, 16> history;
    std::array<LevelWorld::LocalSnapshot, 8> expected;
    LevelWorld::LocalSnapshot observed;
    const auto uploads = replay.device.texturesCreated;
    s32 tick = 0;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.world.saveLocal(saved);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            replay.step(tick + static_cast<s32>(i), 1.0f / 60);
            replay.world.saveLocal(expected[i]);
        }
        REQUIRE(replay.world.restoreLocal(saved));
        for (u32 i = 0; i < distance; ++i) {
            replay.world.capturePresentation();
            replay.world.setPlayerCount(4);
            replay.world.activateTrigger(1, WorldAnimator::Finish::AtOnce);
            replay.world.strikeWall(0, 9999);
            replay.world.explodeObject(3, Vec3{40, 0, 0});
            replay.world.fallingScenery().touch({30, 0, 0}, 10);
            REQUIRE(replay.world.throwItem(replay.device, "COIN", {3, 10, 4}, {4, 2, 1}, 0.1f));
            replay.world.collect(replay.device, std::array{Collector{{80, 0, 0}, 1, 2}});
            replay.world.setAmbientOffset(-0.9f);
            replay.world.update(1.0f / 30);
            replay.world.updateTriggers(1.0f / 30, {});
            replay.world.takeTriggerOpenings();
            replay.world.takeWorldExplosions();
            replay.world.takeRotatorCues();
        }
        REQUIRE(replay.world.restoreLocal(saved));
        CHECK(replay.digest() == native.digest());
        for (u32 i = 0; i < distance; ++i) {
            native.step(tick, 1.0f / 60);
            replay.step(tick, 1.0f / 60);
            replay.world.saveLocal(observed);
            CHECK(replay.world.equalLocal(observed, expected[i]));
            ++tick;
            CHECK(replay.digest() == native.digest());
            CHECK(replay.world.collision().objectTransform(0) ==
                  native.world.collision().objectTransform(0));
            CHECK(replay.world.placedItems().item(0).position ==
                  native.world.placedItems().item(0).position);
            for (f32 const alpha : {-1.0f, 0.5f, 1.0f}) {
                for (auto* fixture : {&native, &replay}) {
                    fixture->device.draws.clear();
                    fixture->world.drawOpaque(fixture->device, Mat4{1}, {}, alpha);
                    fixture->world.drawDeferred(fixture->device, Mat4{1}, {}, alpha);
                }
                REQUIRE(replay.device.draws.size() == native.device.draws.size());
                for (usize j = 0; j < native.device.draws.size(); ++j) {
                    test::checkRecordedDraw(replay.device.draws[j], native.device.draws[j]);
                }
            }
        }
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.world.restoreLocal(history[0]));
    CHECK_FALSE(native.world.equalLocal(history[0], history[0]));
    replay.world.clear();
    CHECK_FALSE(replay.world.restoreLocal(history[0]));
    CHECK_FALSE(replay.world.equalLocal(history[0], history[0]));
}

TEST_CASE("native world equality includes pending child output and latent root controls",
          "[local-rewind][level-world-local-rewind]") {
    Fixture fixture("world-local-hidden-values");
    LevelWorld::LocalSnapshot before;
    LevelWorld::LocalSnapshot changed;
    LevelWorld::LocalSnapshot restored;
    fixture.world.saveLocal(before);
    const auto position = fixture.world.placedItems().item(0).position;
    fixture.world.setItemOpener(0, 3);
    fixture.world.setAmbientOffset(-0.25f);
    fixture.world.saveLocal(changed);
    CHECK(fixture.world.placedItems().item(0).position == position);
    CHECK_FALSE(fixture.world.equalLocal(before, changed));
    REQUIRE(fixture.world.restoreLocal(before));
    fixture.world.saveLocal(restored);
    CHECK(fixture.world.equalLocal(before, restored));

    REQUIRE(fixture.world.explodeObject(3, {40, 0, 0}));
    fixture.world.saveLocal(before);
    const auto explosions = fixture.world.takeWorldExplosions();
    REQUIRE_FALSE(explosions.empty());
    fixture.world.saveLocal(changed);
    CHECK_FALSE(fixture.world.equalLocal(before, changed));
    REQUIRE(fixture.world.restoreLocal(before));
    fixture.world.saveLocal(restored);
    CHECK(fixture.world.equalLocal(before, restored));
    CHECK(fixture.world.takeWorldExplosions() == explosions);
}

TEST_CASE(
    "loaded native worlds report complete owner history cost without diagnostic serialization",
    "[.][assets][level-world-local-timing]") {
    const auto* const name = GENERATE("G2", "G4", "L1", "D2");
    const auto root = test::assetOrSkip(std::string{"LEVELS/LEVEL"} + name + "/WORLDS.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    test::FakeRenderDevice device;
    device.recordDraws = false;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.setPlayerCount(4);
    if (world.skorneArena().size() > 0) {
        world.bossArenaCue({0, 0, 0});
    }
    std::vector<TriggerVisitor> visitors(4);
    if (const auto* start = world.startPoint(0)) {
        for (usize i = 0; i < visitors.size(); ++i) {
            visitors[i].position = start->position + Vec3{static_cast<f32>(i), 0, 0};
        }
    }
    const auto step = [&] {
        world.capturePresentation();
        world.update(1.0f / 60);
        world.updateTriggers(1.0f / 60, visitors);
        world.takeRotatorCues();
        world.takeTriggerCameraCues();
        world.takeTriggerOpenings();
    };
    for (usize i = 0; i < 60; ++i) {
        step();
    }
    std::array<LevelWorld::LocalSnapshot, 16> history;
    for (auto& saved : history) {
        world.saveLocal(saved);
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
    };
    std::array<f64, 256> saves{};
    std::array<f64, 256> restores{};
    std::array<f64, 256> replays{};
    for (usize i = 0; i < saves.size(); ++i) {
        auto& saved = history[i % history.size()];
        auto start = Clock::now();
        world.saveLocal(saved);
        saves[i] = elapsed(start);
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        start = Clock::now();
        const bool restored = world.restoreLocal(saved);
        restores[i] = elapsed(start);
        REQUIRE(restored);
        start = Clock::now();
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        replays[i] = elapsed(start);
    }
    const auto report = [&](std::string_view operation, const auto& samples) {
        const auto timing = test::summarizeNativeTimings(samples);
        REQUIRE(timing);
        std::cout << name << "_level_world_" << operation << " mean_ms=" << timing->mean
                  << " p99_ms=" << timing->p99 << " max_ms=" << timing->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}

TEST_CASE("complete level checkpoints rewind native motion destruction pickups and cue queues",
          "[rollback][level-world-local-rewind]") {
    Fixture host("level-world-checkpoint-host");
    Fixture replay("level-world-checkpoint-replay");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto textures = replay.device.texturesCreated;
    const auto uploads = replay.device.textureUpdates;
    const auto* wall = &host.world.walls().wall(0);
    const auto* rotator = &host.world.rotators().rotator(0);
    bool particleBirth = false;
    bool burst = false;
    bool cartExplosion = false;
    bool wallBroken = false;
    bool carriedPickup = false;
    for (s32 tick = 0; tick < 50; ++tick) {
        CAPTURE(tick, seconds);
        // The replay steps alongside; the host also rewinds each tick and replays it.
        LevelWorld::LocalSnapshot before;
        host.world.saveLocal(before);
        const auto beforeDigest = host.digest();
        REQUIRE(host.world.placedItems().item(0).floor);
        CHECK(host.world.placedItems().item(0).floor->object == 0);
        const auto floorBefore = host.world.collision().objectTransform(0);
        const bool wallSolidBefore = host.world.collision().solid(wall->object);
        host.step(tick, seconds);
        replay.step(tick, seconds);
        const auto expected = host.digest();
        CHECK(replay.digest() == expected);
        particleBirth |= host.world.particles().particleCount() > 0;
        burst |= host.world.placedItems().effectCount() > 0;
        cartExplosion |= host.world.hasWorldExplosions();
        const bool broken = host.world.walls().wall(0).health == 0;
        wallBroken |= broken;
        carriedPickup |= host.world.placedItems().item(0).position.y > PlacedItems::kFloorLift;
        if (broken) {
            CHECK_FALSE(host.world.collision().solid(wall->object));
            CHECK_FALSE(replay.world.collision().solid(wall->object));
        }
        REQUIRE(host.world.restoreLocal(before));
        CHECK(host.digest() == beforeDigest);
        CHECK(host.world.collision().solid(wall->object) == wallSolidBefore);
        CHECK(host.world.collision().objectTransform(0) == floorBefore);
        CHECK(&host.world.walls().wall(0) == wall);
        CHECK(&host.world.rotators().rotator(0) == rotator);
        host.step(tick, seconds);
        CHECK(host.digest() == expected);
        CHECK(host.world.takeWorldExplosions() == replay.world.takeWorldExplosions());
        CHECK(host.world.takeTriggerOpenings() == replay.world.takeTriggerOpenings());
        CHECK(host.world.takeTriggerSettled() == replay.world.takeTriggerSettled());
        CHECK(host.world.takeTriggerCameraCues() == replay.world.takeTriggerCameraCues());
        const auto a = host.world.takeRotatorCues();
        const auto b = replay.world.takeRotatorCues();
        REQUIRE(a.size() == b.size());
        for (usize i = 0; i < a.size(); ++i) {
            CHECK(a[i].kind == b[i].kind);
            CHECK(a[i].rotator == b[i].rotator);
            CHECK(a[i].position == b[i].position);
        }
    }
    CHECK(particleBirth);
    CHECK(burst);
    CHECK(cartExplosion);
    CHECK(wallBroken);
    CHECK(carriedPickup);
    CHECK(replay.device.texturesCreated == textures);
    CHECK(replay.device.textureUpdates == uploads);
}

TEST_CASE("native world digest and rewind include an activated Skorne arena",
          "[rollback][level-world-local-rewind]") {
    Fixture fixture("level-world-checkpoint-skorne");
    LevelWorld::LocalSnapshot dormant;
    fixture.world.saveLocal(dormant);
    const auto before = fixture.digest();
    fixture.world.bossArenaCue({0, 0, 0});
    CHECK(fixture.world.skorneArena().phase() == 1);
    CHECK(fixture.digest() != before);
    LevelWorld::LocalSnapshot active;
    fixture.world.saveLocal(active);
    CHECK_FALSE(fixture.world.equalLocal(dormant, active));
    REQUIRE(fixture.world.restoreLocal(dormant));
    CHECK(fixture.world.skorneArena().phase() == 0);
    CHECK(fixture.digest() == before);
    REQUIRE(fixture.world.restoreLocal(active));
    CHECK(fixture.world.skorneArena().phase() == 1);
}

TEST_CASE("independently loaded native worlds advance alike without uploads",
          "[rollback][level-world-local-rewind][assets]") {
    const std::string name = GENERATE(std::string{"L1"}, std::string{"G1"});
    CAPTURE(name);
    const auto root = test::assetOrSkip("WDATA/SECRET.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(name);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld host;
    LevelWorld restored;
    REQUIRE(host.load(device, root, *level));
    REQUIRE(restored.load(device, root, *level));
    host.setPlayerCount(1);
    restored.setPlayerCount(1);
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    for (s32 tick = 0; tick < 12; ++tick) {
        for (auto* world : {&host, &restored}) {
            world->capturePresentation();
            world->update(1.0f / 60, tick % 4 == 0);
            world->updateTriggers(1.0f / 60, {});
        }
    }
    if (name == "L1") {
        for (const auto object : {1962, 1963}) {
            const auto transform = host.collision().objectTransform(object);
            REQUIRE(transform);
            CHECK(Mat3{*transform} == Mat3{0});
        }
    }
    CHECK(worldDigest(restored, device) == worldDigest(host, device));
    for (s32 tick = 0; tick < 12; ++tick) {
        for (auto* world : {&host, &restored}) {
            world->capturePresentation();
            world->update(1.0f / 60, tick % 3 == 0);
            world->updateTriggers(1.0f / 60, {});
        }
        CHECK(worldDigest(restored, device) == worldDigest(host, device));
    }
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}
} // namespace
