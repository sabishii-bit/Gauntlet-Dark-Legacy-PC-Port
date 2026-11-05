#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/WorldDestruction.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `destruction` now holds exactly the values `saved` holds. */
bool holds(const WorldDestruction& destruction, const WorldDestruction::LocalSnapshot& saved) {
    WorldDestruction::LocalSnapshot now;
    destruction.saveLocal(now);
    return destruction.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    WorldDestruction destruction;

    Fixture() {
        const auto directory = test::sampleLevel("world-destruction-checkpoint");
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"WALL","flags":327680,"position":[10,0,0],"next":1},
          {"name":"WINDOW","position":[0,0,0]}],"animations":[{"object":0,"frames":4,
          "track":{"flags":16,"frames":[0,3],"values":[0,6]}}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        destruction.bind(layout);
        REQUIRE(
            scene.build(layout, models, textures, device, {}, {}, destruction.controlledObjects()));
        animator.bind(layout);
    }
    void step(f32 seconds, bool held = false) {
        animator.step(seconds, scene, held);
        destruction.update(animator.cycleEvents(), scene, collision);
    }
};

TEST_CASE("pending cart blasts advance alike through holds in two loads of a stage",
          "[rollback][world-destruction-local-rewind]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first;
    Fixture second;
    for (int tick = 0; tick < rate / 10; ++tick) {
        first.step(seconds);
        second.step(seconds);
    }
    REQUIRE(first.destruction.hasExplosions());
    REQUIRE(first.destruction.destroyed(0));
    const auto textures = second.device.texturesCreated;
    const auto uploads = second.device.textureUpdates;
    CHECK(test::liveDigest(second.destruction) == test::liveDigest(first.destruction));
    CHECK(second.destruction.takeExplosions() == first.destruction.takeExplosions());
    for (int tick = 0; tick < 24; ++tick) {
        const bool held = tick >= 8 && tick < 12;
        first.step(seconds, held);
        second.step(seconds, held);
        CHECK(test::liveDigest(second.destruction) == test::liveDigest(first.destruction));
        CHECK(second.scene.objectVisible(0) == first.scene.objectVisible(0));
        CHECK(second.collision.solid(0) == first.collision.solid(0));
        CHECK(second.animator.checkpoint() == first.animator.checkpoint());
    }
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == uploads);
}

TEST_CASE("native destruction rosters hash alike across independently loaded stage owners",
          "[rollback][world-destruction-local-rewind][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto* const stage = GENERATE("G2", "D1", "J4");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    LevelWorld first;
    LevelWorld second;
    REQUIRE(first.load(device, root, *level));
    REQUIRE(second.load(device, root, *level));
    WorldDestruction firstLedger;
    WorldDestruction secondLedger;
    firstLedger.bind(first.layout());
    secondLedger.bind(second.layout());
    WorldDestruction::LocalSnapshot state;
    secondLedger.saveLocal(state);
    REQUIRE(secondLedger.restoreLocal(state));
    CHECK(test::liveDigest(secondLedger) == test::liveDigest(firstLedger));
}

TEST_CASE("native destruction history rewinds pending events without emitting collaborator work",
          "[local-rewind][world-destruction-local-rewind]") {
    Fixture reference;
    Fixture corrected;
    struct Snapshot {
        WorldDestruction::LocalSnapshot destruction;
        WorldAnimator::LocalSnapshot animator;
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
    };
    LocalStateHistory<Snapshot> history;
    std::array<WorldDestruction::LocalSnapshot, 8> expected;
    WorldDestruction::LocalSnapshot observed;
    u64 tick = 0;
    for (u64 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(tick, [&](auto& saved) {
            corrected.destruction.saveLocal(saved.destruction);
            corrected.animator.saveLocal(saved.animator);
            corrected.scene.saveLocal(saved.scene);
            corrected.collision.saveLocal(saved.collision);
            return true;
        }));
        const u64 distance = 1 + window % 8;
        const auto* saved = history.find(tick);
        REQUIRE(saved);
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.destruction.saveLocal(expected[offset]);
        }
        REQUIRE(corrected.destruction.restoreLocal(saved->destruction));
        REQUIRE(corrected.animator.restoreLocal(saved->animator));
        REQUIRE(corrected.scene.restoreLocal(saved->scene));
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        for (u64 offset = 0; offset < distance; ++offset) {
            reference.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.step(0.13f);
            corrected.destruction.explode(0, {30, 4, 10}, corrected.scene, corrected.collision);
        }
        const bool visible = corrected.scene.objectVisible(0);
        const bool solid = corrected.collision.solid(0);
        REQUIRE(corrected.destruction.restoreLocal(saved->destruction));
        CHECK(corrected.scene.objectVisible(0) == visible);
        CHECK(corrected.collision.solid(0) == solid);
        CHECK(holds(corrected.destruction, saved->destruction));
        REQUIRE(corrected.animator.restoreLocal(saved->animator));
        REQUIRE(corrected.scene.restoreLocal(saved->scene));
        REQUIRE(corrected.collision.restoreLocal(saved->collision));
        for (u64 offset = 0; offset < distance; ++offset) {
            corrected.step(1.0f / 60, (tick + offset) % 11 == 0);
            corrected.destruction.saveLocal(observed);
            CHECK(corrected.destruction.equalLocal(observed, expected[offset]));
        }
        CHECK(test::liveDigest(corrected.destruction) == test::liveDigest(reference.destruction));
        CHECK(corrected.scene.objectVisible(0) == reference.scene.objectVisible(0));
        CHECK(corrected.scene.worldTransform(0) == reference.scene.worldTransform(0));
        CHECK(corrected.collision.solid(0) == reference.collision.solid(0));
        CHECK(corrected.animator.checkpoint() == reference.animator.checkpoint());
        if (window % 2 == 0) {
            CHECK(corrected.destruction.takeExplosions() == reference.destruction.takeExplosions());
        }
        tick += distance;
    }
    WorldDestruction::LocalSnapshot saved;
    corrected.destruction.saveLocal(saved);
    CHECK_FALSE(reference.destruction.restoreLocal(saved));
    CHECK_FALSE(reference.destruction.equalLocal(saved, saved));
    corrected.destruction.clear();
    CHECK_FALSE(corrected.destruction.restoreLocal(saved));
    CHECK_FALSE(corrected.destruction.equalLocal(saved, saved));
    corrected.destruction.bind(corrected.layout);
    CHECK_FALSE(corrected.destruction.restoreLocal(saved));
}

TEST_CASE("native destruction equality includes undelivered explosions with identical geometry",
          "[local-rewind][world-destruction-local-rewind]") {
    Fixture fixture;
    REQUIRE(fixture.destruction.explode(0, {10, 2, 3}, fixture.scene, fixture.collision));
    WorldDestruction::LocalSnapshot queued;
    WorldDestruction::LocalSnapshot drained;
    WorldDestruction::LocalSnapshot restored;
    fixture.destruction.saveLocal(queued);
    const auto visible = fixture.scene.objectVisible(0);
    const auto solid = fixture.collision.solid(0);
    const auto explosions = fixture.destruction.takeExplosions();
    REQUIRE_FALSE(explosions.empty());
    fixture.destruction.saveLocal(drained);
    CHECK(fixture.scene.objectVisible(0) == visible);
    CHECK(fixture.collision.solid(0) == solid);
    CHECK_FALSE(fixture.destruction.equalLocal(queued, drained));
    REQUIRE(fixture.destruction.restoreLocal(queued));
    fixture.destruction.saveLocal(restored);
    CHECK(fixture.destruction.equalLocal(queued, restored));
    CHECK(fixture.destruction.takeExplosions() == explosions);
}

TEST_CASE("native destruction digest bytes stay pinned after the cart's script and what it saved",
          "[world-destruction-local-rewind][digest-golden]") {
    // The ledger keeps a cart's blast only until the next update, and the cart comes back as its
    // next pass begins, so the script ends on the tick the cart wraps.
    constexpr f32 kSeconds = 1.0f / 30;
    Fixture fixture;
    fixture.step(kSeconds);
    fixture.step(kSeconds);
    WorldDestruction::LocalSnapshot saved;
    fixture.destruction.saveLocal(saved);
    fixture.step(kSeconds);
    REQUIRE(fixture.destruction.hasExplosions());
    REQUIRE(fixture.destruction.destroyed(0));
    // SHA-256 of the ledger's native digest three ticks in, the cart blown up, and of what it
    // saved two ticks in, which netplay peers must hash alike: a literal moves only when what is
    // visited changes on purpose.
    test::checkPinnedDigests("WorldDestruction", test::liveDigest(fixture.destruction),
                             test::savedDigest(fixture.destruction, saved),
                             "9513c334e116baa654b53f34c7d499bc447b9eac209e770279cd352ac2e9390e",
                             "16cd7b62c79fb580d02c20a110fa94b3c2af02e5701592000ad6218cb9680ddd");
}
} // namespace
