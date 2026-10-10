#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/ReplicaPickups.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    WorldLayout layout;
    PlacedItems items;
    PickupResources resources;
    Fixture() {
        const auto root = test::scratchDirectory("replica-pickups");
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
          {"name":"COIN","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":4,"frameRate":30,
           "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]},
          {"name":"TREAS_JUNK","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
          "itemInfos":[{"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2,"armor":0,"collisionType":1}],
          "itemInstances":[]})");
        test::convertModelFixture(root);
        REQUIRE(archive.load(root));
        REQUIRE(layout.load(root));
        items.bind(device, layout, nullptr, std::array{&archive});
        items.setPlayerCount(1);
        REQUIRE(resources.bind(device, items.archives()));
    }
    CombatSnapshot capture(u64 tick = 0) {
        CombatSnapshot state;
        state.motion.epoch = 1;
        state.motion.tick = tick;
        state.motion.cameraContinuity = 1;
        REQUIRE(PickupCapture::append(state, items, resources));
        return state;
    }
};

void sameGeometry(std::span<const test::RecordedDraw> actual,
                  std::span<const test::RecordedDraw> expected) {
    REQUIRE_FALSE(actual.empty());
    REQUIRE(actual.size() == expected.size());
    for (usize draw = 0; draw < actual.size(); ++draw) {
        CAPTURE(draw);
        REQUIRE(actual[draw].vertices.size() == expected[draw].vertices.size());
        CHECK(actual[draw].texture == expected[draw].texture);
        CHECK(actual[draw].state.depthWrite == expected[draw].state.depthWrite);
        CHECK(actual[draw].state.depthTest == expected[draw].state.depthTest);
        CHECK(actual[draw].state.alphaTest == expected[draw].state.alphaTest);
        CHECK(actual[draw].blend() == expected[draw].blend());
        for (usize vertex = 0; vertex < actual[draw].vertices.size(); ++vertex) {
            const auto& a = actual[draw].vertices[vertex];
            const auto& b = expected[draw].vertices[vertex];
            const auto av = actual[draw].transform * Vec4{a.position, 1};
            const auto bv = expected[draw].transform * Vec4{b.position, 1};
            for (s32 axis = 0; axis < 3; ++axis) {
                CHECK(av[axis] == Approx(bv[axis]).margin(0.0001f));
            }
            CHECK(a.color == b.color);
            CHECK(a.uv.x == Approx(b.uv.x).margin(0.0001f));
            CHECK(a.uv.y == Approx(b.uv.y).margin(0.0001f));
        }
    }
}

TEST_CASE("replicated pickups keep stable identities through carrying collection and transmutation",
          "[netplay][replica-pickups]") {
    Fixture f;
    REQUIRE(f.items.place(f.device, "COIN", {7, 0, 0}, nullptr));
    auto first = f.capture();
    REQUIRE(first.pickups.size() == 1);
    const auto id = first.pickups[0].instance;
    CHECK(first.pickups[0].animation.generation > 0);
    REQUIRE(f.items.claim({7, 0, 0}, 1, 1) == 0);
    CHECK(f.capture(1).pickups.empty());
    REQUIRE(f.items.release(0, {9, 0, 0}, {}, nullptr, 0));
    const auto released = f.capture(2);
    REQUIRE(released.pickups.size() == 1);
    CHECK(released.pickups[0].instance == id);
    CHECK(released.pickups[0].continuity > first.pickups[0].continuity);
    REQUIRE(f.items.blast(f.device, {9, 0, 0}, 4, 100).size() == 1);
    const auto changed = f.capture(3);
    REQUIRE(changed.pickups.size() == 1);
    CHECK(changed.pickups[0].instance == id);
    CHECK(changed.pickups[0].resource != first.pickups[0].resource);
    CHECK(changed.pickups[0].animation.generation == 0);
    // A rejected pickup must remain visible; the client cannot award or consume it.
    const std::array collectors{Collector{{9, 0, 0}, 1, 1}};
    CHECK(f.items.collect(f.device, collectors, [](const Pickup&) { return std::optional<s32>{}; })
              .empty());
    CHECK(f.capture(4).pickups.size() == 1);
    REQUIRE(f.items.collect(f.device, collectors).size() == 1);
    CHECK(f.capture(5).pickups.empty());
    REQUIRE(f.items.place(f.device, "COIN", {7, 0, 0}, nullptr));
    const auto dropped = f.capture(6);
    REQUIRE(dropped.pickups.size() == 1);
    CHECK(dropped.pickups[0].instance > id);
}

TEST_CASE("pickup replicas match host meshes without advancing physics animation or collection",
          "[netplay][replica-pickups]") {
    Fixture f;
    WorldCollision collision;
    const std::array<Vec3, 3> floor{Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    collision.build({{{0, 1, 0}, floor, 0, 4}});
    REQUIRE(f.items.throwItem(f.device, "COIN", {7, 5, 0}, {60, 0, 0}, &collision, 2));
    f.items.update(1.0f / 30);
    auto captured = f.capture();
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    const auto packets = CombatReplica::packets(captured);
    REQUIRE(packets);
    for (const auto& packet : *packets) {
        playback.receive(1, packet);
    }
    const auto shown = playback.sample(0);
    REQUIRE(shown);
    ReplicaPickups replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(*shown, f.resources));
    const auto camera = CameraFrame::at({20, 25, 30});
    f.items.draw(f.device, Mat4{1}, {}, &camera);
    const auto expected = f.device.draws;
    const auto textures = f.device.texturesCreated;
    const auto velocity = f.items.item(0).velocity;
    const auto noGrab = f.items.item(0).noGrabSeconds;
    for (usize frame = 0; frame < 3; ++frame) {
        f.device.draws.clear();
        replica.draw(f.device, f.resources, Mat4{1}, {}, camera, TreeModel::Pass::Opaque);
        replica.draw(f.device, f.resources, Mat4{1}, {}, camera, TreeModel::Pass::Blended);
        sameGeometry(f.device.draws, expected);
    }
    CHECK(f.device.texturesCreated == textures);
    CHECK(f.items.item(0).velocity == velocity);
    CHECK(f.items.item(0).noGrabSeconds == noGrab);
    CHECK(CombatPacket::encode(f.capture()) == CombatPacket::encode(captured));
    f.items.discard(0);
    REQUIRE(replica.show(f.capture(1), f.resources));
    CHECK(replica.count() == 0);
    CHECK_FALSE(replica.show(captured, f.resources));
    REQUIRE(replica.begin(2));
    CHECK_FALSE(replica.show(captured, f.resources));
}

TEST_CASE("pickup capture follows live floors and marks container and platform cuts",
          "[netplay][replica-pickups]") {
    Fixture f;
    WorldCollision collision;
    const std::array<Vec3, 3> floor{Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    collision.build({{{0, 1, 0}, floor, 4, 4100}});
    collision.setMovingObjects(std::array<s32, 1>{4});
    REQUIRE(f.items.place(f.device, "COIN", {7, 0, 0}, &collision));
    REQUIRE(f.items.item(0).floor);
    const auto first = f.capture(10);
    collision.setObjectTransform(4, glm::translate(Mat4{1}, Vec3{0, -4, 0}));
    f.items.syncFloors();
    const auto next = f.capture(14);
    CHECK(next.pickups[0].placement[3].y == Approx(-3.9f));
    CHECK(next.pickups[0].continuity == first.pickups[0].continuity);
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    for (const auto& snapshot : {first, next}) {
        const auto packets = CombatReplica::packets(snapshot);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            playback.receive(1, packet);
        }
    }
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    CHECK(middle->pickups[0].placement[3].y == Approx(-1.9f));
    f.items.attach(0, glm::translate(Mat4{1}, Vec3{7, -3, 0}), true);
    const auto attached = f.capture(15);
    CHECK(attached.pickups[0].continuity > next.pickups[0].continuity);
    f.items.attach(0, glm::translate(Mat4{1}, Vec3{7, -2, 0}), true);
    CHECK(f.capture(16).pickups[0].continuity == attached.pickups[0].continuity);
    f.items.snapPresentation();
    CHECK(f.capture(17).pickups[0].continuity > attached.pickups[0].continuity);
}

TEST_CASE("pickup capacity and resource validation preserve the last complete view",
          "[netplay][replica-pickups]") {
    Fixture f;
    REQUIRE(f.items.place(f.device, "COIN", {}, nullptr));
    auto captured = f.capture();
    const auto original = CombatPacket::encode(captured);
    const PickupResources missing;
    CHECK_FALSE(PickupCapture::append(captured, f.items, missing));
    CHECK(CombatPacket::encode(captured) == original);
    ReplicaPickups replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(captured, f.resources));
    auto bad = captured;
    bad.pickups[0].resource = 65535;
    CHECK_FALSE(replica.show(bad, f.resources));
    bad = captured;
    bad.pickups[0].animation.sequence = 1;
    CHECK_FALSE(replica.show(bad, f.resources));
    bad = captured;
    bad.pickups[0].animation.frame = 4;
    CHECK_FALSE(replica.show(bad, f.resources));
    CHECK(replica.count() == 1);
    CHECK_FALSE(f.resources.bind(f.device, std::array{&f.archive, &f.archive}));
    CHECK(f.resources.accepts(captured.pickups[0]));
    for (usize i = 0; i < CombatSnapshot::kMaxPickups; ++i) {
        REQUIRE(f.items.place(f.device, "COIN", {}, nullptr));
    }
    CHECK_FALSE(PickupCapture::append(captured, f.items, f.resources));
    CHECK(CombatPacket::encode(captured) == original);
}

TEST_CASE("native pickup sprites and model shadows retain host texture phase and occlusion",
          "[netplay][replica-pickups][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G2");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto& items = world.placedItems();
    for (usize i = 0; i < items.size(); ++i) {
        world.discardItem(i);
    }
    PickupResources resources;
    REQUIRE(resources.bind(device, items.archives()));
    for (const auto* name : {"BREATHEF_ICON", "APPLE", "CHERRY", "ACIDICON"}) {
        REQUIRE(world.placeItem(device, name, {}));
    }
    const auto camera = CameraFrame::at({20, 25, 30});
    ReplicaPickups replica;
    REQUIRE(replica.begin(1));
    for (u64 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick);
        world.update(1.0f / 30);
        CombatSnapshot state;
        state.motion.epoch = state.motion.cameraContinuity = 1;
        state.motion.tick = tick;
        REQUIRE(PickupCapture::append(state, items, resources));
        REQUIRE(state.pickups.size() == 4);
        REQUIRE(replica.show(state, resources));
        device.draws.clear();
        items.draw(device, Mat4{1}, {}, &camera, TreeModel::Pass::Opaque);
        items.draw(device, Mat4{1}, {}, &camera, TreeModel::Pass::Blended);
        const auto expected = device.draws;
        device.draws.clear();
        replica.draw(device, resources, Mat4{1}, {}, camera, TreeModel::Pass::Opaque);
        replica.draw(device, resources, Mat4{1}, {}, camera, TreeModel::Pass::Blended);
        sameGeometry(device.draws, expected);
    }
}

TEST_CASE("every native level fits the pickup roster for four players and future drops",
          "[netplay][pickup-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::set<std::string> names{"L1"};
    for (const auto& realm : catalog.realms()) {
        names.insert(realm.levels.begin(), realm.levels.end());
    }
    REQUIRE(names.size() > 1);
    usize maximum = 0;
    std::string largest;
    for (const auto& name : names) {
        CAPTURE(name);
        const auto level = name == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(name);
        REQUIRE(level);
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root, *level));
        const auto& items = world.placedItems();
        world.setPlayerCount(4);
        PickupResources resources;
        REQUIRE(resources.bind(device, items.archives()));
        world.update(1.0f / 30);
        CombatSnapshot state;
        state.motion.epoch = state.motion.cameraContinuity = 1;
        state.geometry = world.scene().geometry();
        REQUIRE(PickupCapture::append(state, items, resources));
        REQUIRE(CombatReplica::packets(state));
        if (items.visibleCount() > maximum) {
            maximum = items.visibleCount();
            largest = name;
        }
        CHECK(state.pickups.size() == items.visibleCount());
    }
    WARN("Pickup census: " << names.size() << " levels; largest entry roster " << largest
                           << " with " << maximum << " visible pickups for four players");
    CHECK(maximum > 0);
    CHECK(maximum < CombatSnapshot::kMaxPickups);
}
} // namespace
