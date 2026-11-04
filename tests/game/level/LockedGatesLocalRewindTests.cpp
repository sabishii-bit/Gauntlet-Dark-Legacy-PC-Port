#include <array>
#include <span>
#include <string_view>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/LockedGates.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkEvents(std::span<const GateEvent> actual, std::span<const GateEvent> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        const auto& a = actual[index];
        const auto& b = expected[index];
        CHECK((std::tie(a.kind, a.gate, a.visitor, a.position) ==
               std::tie(b.kind, b.gate, b.visitor, b.position)));
    }
}

/** Whether `gates` now hold exactly the values `saved` holds. */
bool holds(const LockedGates& gates, const LockedGates::LocalSnapshot& saved) {
    LockedGates::LocalSnapshot now;
    gates.saveLocal(now);
    return gates.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    WorldCollision collision;
    LockedGates gates;

    explicit Fixture(std::string_view name, bool moving = false, bool unseen = false) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":7,"subtype":1,"name":"GATE","radius":1,"height":3},
            {"type":7,"subtype":2,"name":"GATE","radius":3,"height":1,"collisionType":4}],
          "itemInstances":[
            {"info":0,"position":[0,0,0]},
            {"info":0,"position":[20,0,0],"minPlayers":2},
            {"info":1,"position":[40,0,0],"collision":[
              {"normal":[0,1,0],"vertices":[[-3,0,-3],[3,0,-3],[0,0,3]]}]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"GATE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"rate":30},
             {"name":"ACTIVE","frames":45,"rate":30,
              "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
             {"name":"OPEN","frames":0}]}]})");
        REQUIRE(layout.load(directory));
        if (!unseen) {
            REQUIRE(items.load(directory));
        }
        if (moving) {
            collision.build(
                {{{0, 1, 0}, {Vec3{-8, 0, -8}, Vec3{8, 0, -8}, Vec3{0, 0, 8}}, 0, 4100}});
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(gates.bind(device, layout, AssetScope(items), moving ? &collision : nullptr));
        gates.setPlayerCount(2);
        REQUIRE(gates.size() == 3);
        REQUIRE_FALSE(gates.gate(2).blocksPassage);
    }
    /** The gates' native digest over their own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        test::registerItemStage(resources, items, layout, &collision, device);
        return test::liveDigest(gates, resources);
    }
};

TEST_CASE("gate support checkpoint preserves all local offsets even without figure art",
          "[gates-local-rewind]") {
    const bool unseen = GENERATE(false, true);
    Fixture fixture("gate-checkpoint-support", true, unseen);
    REQUIRE(fixture.gates.gate(0).support.object() == 0);
    fixture.gates.capturePresentation();
    LockedGates::LocalSnapshot before;
    fixture.gates.saveLocal(before);
    REQUIRE(fixture.gates.gate(0).figure.hasFigure() == !unseen);
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{0, -10, 3}) * glm::rotate(Mat4{1}, 0.4f, Vec3{0, 1, 0});
    fixture.collision.setObjectTransform(0, moved);
    fixture.collision.setSolid(0, false);
    fixture.gates.syncFloors();
    fixture.gates.capturePresentation();
    LockedGates::LocalSnapshot expected;
    fixture.gates.saveLocal(expected);
    REQUIRE_FALSE(fixture.gates.equalLocal(expected, before));
    REQUIRE(fixture.gates.restoreLocal(before));
    CHECK(holds(fixture.gates, before));
    CHECK(fixture.collision.objectTransform(0) == moved);
    fixture.gates.syncFloors();
    fixture.gates.capturePresentation();
    CHECK(holds(fixture.gates, expected));
    fixture.collision.clear();
    fixture.gates.syncFloors();
    REQUIRE(fixture.gates.gate(0).support.object() == -1);
    REQUIRE(fixture.gates.restoreLocal(expected));
    CHECK(holds(fixture.gates, expected));
}

TEST_CASE("native campaign gates retain the complete trusted geometry and figure roster",
          "[gates-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto* const name = GENERATE("G1", "G3", "E1");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    LockedGates gates;
    REQUIRE(gates.bind(device, world.layout(), world.itemScope(), &world.collision()));
    LockedGates::LocalSnapshot before;
    gates.saveLocal(before);
    const auto uploads = device.texturesCreated;
    gates.setPlayerCount(4);
    gates.capturePresentation();
    gates.update(1, 1.0f / 60, {});
    REQUIRE(gates.restoreLocal(before));
    CHECK(holds(gates, before));
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("gate local values undo speculative unlocking and moving support without rebinding",
          "[local-rewind][fixture-local-rewind][gates-local-rewind]") {
    const bool unseen = GENERATE(false, true);
    Fixture native("gate-local-native", true, unseen);
    Fixture replay("gate-local-replay", true, unseen);
    std::array<LockedGates::LocalSnapshot, 16> history;
    std::array<LockedGates::LocalSnapshot, 8> expectedLocal;
    LockedGates::LocalSnapshot observed;
    WorldCollision::LocalSnapshot floor;
    const auto uploads = replay.device.texturesCreated;
    const auto step = [](Fixture& fixture, s32 tick, bool wrong) {
        const auto phase = static_cast<f32>(tick);
        fixture.collision.setObjectTransform(
            0, glm::translate(Mat4{1}, Vec3{0, (wrong ? 0.2f : -0.01f) * phase, 0}));
        fixture.gates.setPlayerCount(wrong ? 1 : 2);
        fixture.gates.capturePresentation();
        const auto support = fixture.collision.objectTransform(0);
        REQUIRE(support.has_value());
        const Vec3 spot{*support * Vec4{0, 0, 0, 1}};
        const std::array party{ChestVisitor{spot, 1, wrong || tick >= 5 ? 1 : 0},
                               ChestVisitor{Vec3{20, 0, 0}, 1, !wrong && tick >= 12 ? 1 : 0}};
        return fixture.gates.update(wrong ? 3 : 1, wrong ? 0.05f : 1.0f / 60, party);
    };
    bool refused = false;
    bool unlocked = false;
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        const auto distance = static_cast<s32>(window % 8 + 1);
        CAPTURE(window, distance, unseen);
        auto& saved = history[window % history.size()];
        replay.gates.saveLocal(saved);
        replay.collision.saveLocal(floor);
        CHECK_FALSE(native.gates.canRestoreLocal(saved));
        CHECK_FALSE(native.gates.equalLocal(saved, saved));
        for (s32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.gates.saveLocal(expectedLocal[static_cast<usize>(i)]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.gates.restoreLocal(saved));
        for (s32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        replay.gates.draw(replay.device, Mat4{1}, {}, 0.4f);
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.gates.restoreLocal(saved));
        CHECK(holds(replay.gates, saved));
        for (s32 i = 0; i < distance; ++i) {
            const auto expected = step(native, tick + i, false);
            checkEvents(step(replay, tick + i, false), expected);
            for (const auto& event : expected) {
                refused |= event.kind == GateEvent::Kind::Refused;
                unlocked |= event.kind == GateEvent::Kind::Unlocked;
            }
            CHECK(replay.digest() == native.digest());
            replay.gates.saveLocal(observed);
            CHECK(replay.gates.equalLocal(observed, expectedLocal[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(refused);
    CHECK(unlocked);
    CHECK(replay.gates.gate(0).state == LockedGates::kOpen);
    CHECK(replay.device.texturesCreated == uploads);
    auto& saved = history.front();
    replay.gates.saveLocal(saved);
    replay.gates.clear();
    CHECK_FALSE(replay.gates.restoreLocal(saved));
    REQUIRE(replay.gates.bind(replay.device, replay.layout, AssetScope(replay.items),
                              &replay.collision));
    CHECK_FALSE(replay.gates.restoreLocal(saved));
    CHECK_FALSE(replay.gates.equalLocal(saved, saved));
}

TEST_CASE("native gate equality includes refusal clocks without a visible geometry change",
          "[local-rewind][gates-local-rewind][native-equality]") {
    Fixture fixture("gate-local-equality");
    LockedGates::LocalSnapshot initial;
    LockedGates::LocalSnapshot refused;
    LockedGates::LocalSnapshot restored;
    CHECK_FALSE(fixture.gates.equalLocal(initial, initial));
    fixture.gates.saveLocal(initial);
    const auto figure = fixture.gates.gate(0).figure;
    const auto box = fixture.gates.gate(0).box;
    const std::array party{ChestVisitor{Vec3{0, 0, 0}, 1, 0}};
    const auto events = fixture.gates.update(0, 0, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events.front().kind == GateEvent::Kind::Refused);
    fixture.gates.saveLocal(refused);
    CHECK(fixture.gates.gate(0).figure.equalLocal(figure));
    CHECK(fixture.gates.gate(0).box == box);
    CHECK_FALSE(fixture.gates.equalLocal(initial, refused));
    REQUIRE(fixture.gates.restoreLocal(initial));
    fixture.gates.saveLocal(restored);
    CHECK(fixture.gates.equalLocal(initial, restored));
}
} // namespace
