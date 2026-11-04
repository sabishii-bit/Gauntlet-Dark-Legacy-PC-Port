#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/NativeStateResources.h"
#include "game/level/PlacedItems.h"
#include "game/level/StageRegistry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `items` now hold exactly the values `saved` holds. */
bool holds(const PlacedItems& items, const PlacedItems::LocalSnapshot& saved) {
    PlacedItems::LocalSnapshot now;
    items.saveLocal(now);
    return items.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive archive;
    WorldCollision collision;
    PlacedItems items;

    explicit Fixture(std::string_view tag, bool moving = false) {
        const auto root = test::scratchDirectory(tag);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"FLAME0","file":"white.png","width":2,"height":2},
          {"name":"FLAME1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"FLAME0",
            "particleLife":[0.1,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"FLAME","texture":1,"source":1,"frames":2,"rate":1}],
          "trees":[
           {"name":"COIN","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
             {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30,
              "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]},
           {"name":"GEMORANGE","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"TREAS_JUNK","nodes":[
             {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
           {"name":"GETGEMORANGE","nodes":[
             {"name":"SPARK","particle":0,"parent":-1,"position":[0,1,0]}],
            "sequences":[{"name":"BURST","frames":6,"frameRate":30}]}]})");
        writeTextFile(root / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
          "itemInfos":[
            {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2,"armor":0,"collisionType":1},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2}],
          "itemInstances":[{"info":0,"position":[0,0,0]},
            {"info":1,"position":[20,0,0]}, {"info":1,"position":[40,0,0],"minPlayers":2}]})");
        test::convertModelFixture(root);
        REQUIRE(layout.load(root));
        REQUIRE(archive.load(root));
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}}, 0, 4}});
        if (moving) {
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(items.bind(device, layout, &collision, std::array{&archive}));
        items.setPlayerCount(2);
    }
    /** The pickups' native digest over their own stage, so two fixtures can be compared. It
     * leaves out what only drawing derives. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        test::registerItemStage(resources, archive, layout, &collision, device);
        return test::liveDigest(items, resources);
    }
    void takeGem(usize index) {
        const auto result =
            items.collect(device, std::array{Collector{items.item(index).position, 1, 1}});
        REQUIRE(result.size() == 1);
        REQUIRE(result.front().item == index);
    }
};

TEST_CASE("native pickup history restores corrected births flight rewards and moving floors",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture native("pickup-local-native", true);
    Fixture replay("pickup-local-replay", true);
    std::array<PlacedItems::LocalSnapshot, 16> history;
    std::array<PlacedItems::LocalSnapshot, 8> expected;
    PlacedItems::LocalSnapshot observed;
    WorldCollision::LocalSnapshot floor;
    const auto step = [](Fixture& fixture, u32 tick, bool wrong) {
        fixture.items.capturePresentation();
        fixture.collision.setObjectTransform(
            0, glm::translate(Mat4{1},
                              Vec3{0, wrong ? -20.0f : static_cast<f32>(tick % 20) * 0.2f, 0}));
        fixture.items.setPlayerCount(wrong ? 1 : 2);
        if (tick % 17 == 0 || wrong) {
            REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {5, 8, 0},
                                            wrong ? Vec3{-20, 5, 0} : Vec3{2, 8, 0},
                                            &fixture.collision, 0.3f));
        }
        if (tick % 29 == 0) {
            fixture.items.hideCrystals();
        }
        fixture.items.reveal(1.0f / 60);
        fixture.items.attach(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}), tick % 7 < 3);
        fixture.items.setOpener(0, static_cast<s32>(tick % 4));
        if (tick % 23 == 0 || wrong) {
            fixture.items.blast(fixture.device, Vec3{5, 8, 0}, 30, 100);
        }
        if (tick % 31 == 0) {
            if (const auto item = fixture.items.claim({5, 8, 0}, 12, 16)) {
                REQUIRE(
                    fixture.items.release(*item, {3, 7, 0}, {2, 3, 0}, &fixture.collision, 0.1f));
            }
        }
        if (wrong) {
            fixture.items.discard(0);
        }
        const auto pickups = fixture.items.collect(
            fixture.device, std::array{Collector{Vec3{tick < 80 ? 20.0f : 40.0f, 0, 0}, 2, 3}});
        fixture.items.update(1.0f / 60);
        // Drawing is interleaved with simulation to expose stale shared caches.
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
        return pickups;
    };
    u32 tick = 0;
    const auto uploads = replay.device.texturesCreated;
    for (u32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.items.saveLocal(saved);
        replay.collision.saveLocal(floor);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.items.saveLocal(expected[i]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.items.restoreLocal(saved));
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.items.restoreLocal(saved));
        CHECK(replay.digest() == native.digest());
        for (u32 i = 0; i < distance; ++i) {
            const auto a = step(native, tick + i, false);
            const auto b = step(replay, tick + i, false);
            replay.items.saveLocal(observed);
            CHECK(replay.items.equalLocal(observed, expected[i]));
            REQUIRE(a.size() == b.size());
            for (usize j = 0; j < a.size(); ++j) {
                CHECK(a[j].item == b[j].item);
                CHECK(a[j].collector == b[j].collector);
                CHECK(a[j].amount == b[j].amount);
                CHECK(a[j].position == b[j].position);
            }
            // Hidden/retired items do not redraw their derived pose cache. A
            // discarded speculative draw may therefore leave different cache
            // contents, but not different continuation values or visible draws.
            // Complete same-owner native equality is checked above; the digest
            // compares the two loads without their draw-only caches.
            CHECK(replay.digest() == native.digest());
            REQUIRE(replay.device.draws.size() == native.device.draws.size());
            for (usize j = 0; j < native.device.draws.size(); ++j) {
                test::checkRecordedDraw(replay.device.draws[j], native.device.draws[j]);
            }
        }
        tick += distance;
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.items.restoreLocal(history[0]));
    CHECK_FALSE(native.items.equalLocal(history[0], history[0]));
    replay.items.clear();
    CHECK_FALSE(replay.items.restoreLocal(history[0]));
    CHECK_FALSE(replay.items.equalLocal(history[0], history[0]));
}

TEST_CASE(
    "pickup rewind derives interpolated poses after corrected births without saving draw caches",
    "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-draw-cache");
    fixture.items.capturePresentation();
    fixture.items.update(1.0f / 60);
    PlacedItems::LocalSnapshot beforeDraw;
    fixture.items.saveLocal(beforeDraw);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
    const auto expected = fixture.device.draws;
    REQUIRE_FALSE(expected.empty());
    PlacedItems::LocalSnapshot afterDraw;
    fixture.items.saveLocal(afterDraw);
    CHECK(fixture.items.equalLocal(beforeDraw, afterDraw));
    CHECK(holds(fixture.items, beforeDraw));

    for (const auto* saved : {&beforeDraw, &afterDraw}) {
        REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {8, 10, 0}, {2, 4, 0},
                                        &fixture.collision, 0.5f));
        fixture.items.capturePresentation();
        fixture.items.update(7.0f / 60);
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.91f);
        const auto uploads = fixture.device.texturesCreated;
        REQUIRE(fixture.items.restoreLocal(*saved));
        CHECK(holds(fixture.items, beforeDraw));
        fixture.device.draws.clear();
        fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.37f);
        REQUIRE(fixture.device.draws.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            test::checkRecordedDraw(fixture.device.draws[i], expected[i]);
        }
        CHECK(fixture.device.texturesCreated == uploads);
    }
}

TEST_CASE("native pickup history refuses a changed collision owner before mutating values",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-collision-guard");
    PlacedItems::LocalSnapshot saved;
    fixture.items.saveLocal(saved);
    WorldCollision other;
    other.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}}, 0, 4}});
    REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {5, 8, 0}, {2, 4, 0}, &other, 0.5f));
    const auto before = fixture.items.size();
    CHECK_FALSE(fixture.items.canRestoreLocal(saved));
    CHECK_FALSE(fixture.items.restoreLocal(saved));
    CHECK(fixture.items.size() == before);
}

TEST_CASE("native pickup equality distinguishes loot ownership without visible movement",
          "[local-rewind][placed-items-local-rewind]") {
    Fixture fixture("pickup-local-owner-values");
    PlacedItems::LocalSnapshot before;
    PlacedItems::LocalSnapshot after;
    PlacedItems::LocalSnapshot restored;
    fixture.items.saveLocal(before);
    const auto position = fixture.items.item(0).position;
    const auto value = fixture.items.item(0).value;
    fixture.items.setOpener(0, 3);
    fixture.items.saveLocal(after);
    CHECK(fixture.items.item(0).position == position);
    CHECK(fixture.items.item(0).value == value);
    CHECK_FALSE(fixture.items.equalLocal(before, after));
    REQUIRE(fixture.items.restoreLocal(before));
    fixture.items.saveLocal(restored);
    CHECK(fixture.items.equalLocal(before, restored));
}

TEST_CASE(
    "prepared pickup undo restores dynamic append replacement and bursts without resource creation",
    "[placed-items-local-rewind]") {
    Fixture fixture("pickup-checkpoint-undo");
    fixture.items.update(1.0f / 30);
    fixture.items.draw(fixture.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.5f);
    PlacedItems::LocalSnapshot baseline;
    fixture.items.saveLocal(baseline);
    REQUIRE(fixture.items.presentationPoseOf(0) != nullptr);
    const auto drawnPose = fixture.items.presentationPoseOf(0)->checkpoint();
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(fixture.items.throwItem(fixture.device, "COIN", {4, 5, 0}, {1, 2, 3},
                                    &fixture.collision, 0.4f));
    REQUIRE(fixture.items.blast(fixture.device, fixture.items.item(0).position, 2, 100).size() ==
            1);
    REQUIRE(fixture.items.item(0).name == "TREAS_JUNK");
    fixture.takeGem(1);
    fixture.items.update(1.0f / 30);
    PlacedItems::LocalSnapshot changed;
    fixture.items.saveLocal(changed);
    REQUIRE_FALSE(fixture.items.equalLocal(changed, baseline));
    // The cached old COIN pose survives the native replacement.
    REQUIRE(fixture.items.presentationPoseOf(0) != nullptr);
    const auto cachedPose = fixture.items.presentationPoseOf(0)->checkpoint();
    REQUIRE(cachedPose);
    REQUIRE(fixture.items.item(0).figure != nullptr);
    REQUIRE(cachedPose->tree != fixture.items.item(0).figure->name);
    CHECK(cachedPose->tree == "COIN");
    CHECK(fixture.items.item(0).figure->name == "TREAS_JUNK");
    CHECK(cachedPose == drawnPose);
    REQUIRE(fixture.items.restoreLocal(baseline));
    CHECK(holds(fixture.items, baseline));
    REQUIRE(fixture.items.restoreLocal(changed));
    CHECK(holds(fixture.items, changed));
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE(
    "pickup restoration preserves carried state floor-local attachment and pending presentation",
    "[placed-items-local-rewind]") {
    Fixture fixture("pickup-checkpoint-support", true);
    REQUIRE(fixture.items.item(0).floor);
    fixture.items.capturePresentation();
    PlacedItems::LocalSnapshot baseline;
    fixture.items.saveLocal(baseline);
    fixture.collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 7, 0}));
    fixture.items.syncFloors();
    CHECK_FALSE(holds(fixture.items, baseline));
    REQUIRE(fixture.items.restoreLocal(baseline));
    CHECK(holds(fixture.items, baseline)); // no implicit floor resnap during restore
    fixture.items.syncFloors();
    REQUIRE(fixture.items.claim(fixture.items.item(0).position, 1, 1) == 0);
    PlacedItems::LocalSnapshot carried;
    fixture.items.saveLocal(carried);
    REQUIRE(fixture.items.item(0).carried);
    REQUIRE_FALSE(fixture.items.item(0).floor);
    REQUIRE(fixture.items.release(0, {0, 10, 0}, {2, 3, 4}, &fixture.collision, 0.5f));
    REQUIRE(fixture.items.restoreLocal(carried));
    CHECK(holds(fixture.items, carried));
}

TEST_CASE("native pickup rewind resumes collection bursts without uploads",
          "[placed-items-local-rewind][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive powerups;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    REQUIRE(powerups.load(root / "POWERUPS"));
    PlacedItems items;
    REQUIRE(items.bind(device, layout, nullptr, std::array{&powerups}));
    items.setPlayerCount(1);
    PlacedItems::LocalSnapshot baseline;
    items.saveLocal(baseline);
    usize gem = items.size();
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "GEMORANGE") {
            gem = i;
            break;
        }
    }
    REQUIRE(gem < items.size());
    REQUIRE(items.collect(device, std::array{Collector{items.item(gem).position, 1, 1}}).size() ==
            1);
    items.update(1.0f / 60);
    PlacedItems::LocalSnapshot active;
    items.saveLocal(active);
    REQUIRE(items.effectCount() > 0);
    REQUIRE(items.bursts().size() > 0);
    const auto uploads = device.texturesCreated;
    REQUIRE(items.restoreLocal(baseline));
    CHECK(holds(items, baseline));
    REQUIRE(items.restoreLocal(active));
    CHECK(holds(items, active));
    items.update(1.0f / 60);
    PlacedItems::LocalSnapshot next;
    items.saveLocal(next);
    REQUIRE(items.restoreLocal(active));
    items.update(1.0f / 60);
    CHECK(holds(items, next));
    CHECK(device.texturesCreated == uploads);
}
} // namespace
