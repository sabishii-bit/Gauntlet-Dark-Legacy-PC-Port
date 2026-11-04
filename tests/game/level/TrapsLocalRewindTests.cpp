#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
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
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/ItemFigure.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/StageRegistry.h"
#include "game/level/Traps.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** Whether `traps` now hold exactly the values `saved` holds. */
bool holds(const Traps& traps, const Traps::LocalSnapshot& saved) {
    Traps::LocalSnapshot now;
    traps.saveLocal(now);
    return traps.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive items;
    WorldLayout layout;
    WorldCollision collision;
    Traps traps;
    explicit Fixture(bool moving = false) {
        const auto root = test::scratchDirectory("traps-checkpoint");
        writeTextFile(root / "tri.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(
            root / "objects.json",
            R"({"objects":[{"index":0,"name":"TRI","file":"tri.obj","meshTriangles":1}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(
            root / "textures.json",
            R"({"bitmaps":[{"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[0.2,0.1],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
          {"name":"SPIKES","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
            {"name":"SPRAY","parent":0,"position":[0,1,0],"particle":0}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]},
          {"name":"SPIKES_D","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
            {"name":"SPRAY","parent":0,"position":[0,1,0],"particle":0}],
           "sequences":[{"name":"IDLE","frames":2,"frameRate":30}]},
          {"name":"BARE","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]}]})");
        writeTextFile(root / "world.json",
                      R"({"objects":[{"name":"TRI","position":[0,0,0],"flags":4100}],"itemInfos":[
          {"type":8,"subtype":0,"name":"SPIKES","radius":2,"height":3,"value":20,"activeOff":-4},
          {"type":8,"subtype":3,"name":"BARE","radius":2,"height":3,"value":10,"activeOff":-3}],
          "itemInstances":[{"info":0,"position":[0,0,0]},{"info":1,"position":[20,0,0]}]})");
        test::convertModelFixture(root);
        REQUIRE(items.load(root));
        REQUIRE(layout.load(root));
        if (moving) {
            collision.build(
                {{{0, 1, 0}, {Vec3{-32, 0, -32}, Vec3{32, 0, 32}, Vec3{32, 0, -32}}, 0, 4100},
                 {{0, 1, 0}, {Vec3{-32, 0, -32}, Vec3{-32, 0, 32}, Vec3{32, 0, 32}}, 0, 4100}});
            collision.setMovingObjects(std::array<s32, 1>{0});
            collision.setObjectTransform(0, Mat4{1});
        }
        REQUIRE(traps.bind(device, layout, AssetScope(items), moving ? &collision : nullptr, 7123,
                           1.25f, 1.5f));
    }
    /** The traps' native digest over their own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        test::registerItemStage(resources, items, layout, &collision, device);
        return test::liveDigest(traps, resources);
    }
};
void compareHits(const std::vector<TrapHit>& a, const std::vector<TrapHit>& b) {
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].trap == b[i].trap);
        CHECK(a[i].victim == b[i].victim);
        CHECK(a[i].damage == b[i].damage);
        CHECK(a[i].subtype == b[i].subtype);
        CHECK(a[i].pierces == b[i].pierces);
        CHECK(a[i].position == b[i].position);
        CHECK(a[i].impact.flags == b[i].impact.flags);
        CHECK(a[i].impact.direction == b[i].impact.direction);
    }
}
TEST_CASE("trap checkpoints switch between original disarmed and vanished figures without uploads",
          "[rollback][traps-local-rewind]") {
    Fixture first;
    Traps::LocalSnapshot initial;
    first.traps.saveLocal(initial);
    for (usize i = 0; i < first.traps.size(); ++i) {
        REQUIRE(
            first.traps.disarm(i, first.device, first.layout, AssetScope(first.items), nullptr));
    }
    CHECK(first.traps.trap(0).disarmed);
    CHECK_FALSE(first.traps.trap(0).gone);
    CHECK(first.traps.trap(1).gone);
    CHECK_FALSE(first.traps.trap(1).shown);
    Traps::LocalSnapshot state;
    first.traps.saveLocal(state);
    REQUIRE(first.traps.restoreLocal(initial));
    const auto textures = first.device.texturesCreated;
    const auto uploads = first.device.textureUpdates;
    const auto* address = &first.traps.trap(0);
    REQUIRE(first.traps.restoreLocal(state));
    CHECK(holds(first.traps, state));
    CHECK(&first.traps.trap(0) == address);
    REQUIRE(first.traps.restoreLocal(initial));
    CHECK(holds(first.traps, initial));
    CHECK_FALSE(first.traps.trap(0).disarmed);
    CHECK(first.traps.trap(1).shown);
    CHECK(first.device.texturesCreated == textures);
    CHECK(first.device.textureUpdates == uploads);
}
TEST_CASE("disarming a striking trap without alternate art retains its frozen native pose",
          "[rollback][traps-local-rewind]") {
    Fixture first;
    Fixture second;
    for (s32 tick = 0; tick < 1000 && first.traps.trap(1).action != 2; ++tick) {
        first.traps.update(1, 1.0f / 60, {});
        second.traps.update(1, 1.0f / 60, {});
    }
    REQUIRE(first.traps.trap(1).action == 2);
    first.traps.update(1, 1.0f / 60, {});
    second.traps.update(1, 1.0f / 60, {});
    REQUIRE(first.traps.trap(1).action == 2);
    Traps::LocalSnapshot striking;
    Traps::LocalSnapshot secondStriking;
    first.traps.saveLocal(striking);
    second.traps.saveLocal(secondStriking);
    const ItemFigure strikingFigure = first.traps.trap(1).figure;
    const auto textures = second.device.texturesCreated;
    REQUIRE(first.traps.disarm(1, first.device, first.layout, AssetScope(first.items), nullptr));
    CHECK(first.traps.trap(1).action == Traps::kResting);
    CHECK(first.traps.trap(1).gone);
    CHECK(first.traps.trap(1).figure == strikingFigure);
    REQUIRE(
        second.traps.disarm(1, second.device, second.layout, AssetScope(second.items), nullptr));
    CHECK(second.digest() == first.digest());
    const std::array<TrapVictim, 1> victims{{{{20, 1, 0}, 0.75f, 1.5f}}};
    for (s32 tick = 0; tick < 60; ++tick) {
        compareHits(first.traps.update(1, 1.0f / 60, victims),
                    second.traps.update(1, 1.0f / 60, victims));
        CHECK(second.digest() == first.digest());
    }
    REQUIRE(first.traps.restoreLocal(striking));
    REQUIRE(second.traps.restoreLocal(secondStriking));
    CHECK_FALSE(second.traps.trap(1).disarmed);
    for (s32 tick = 0; tick < 60; ++tick) {
        compareHits(first.traps.update(1, 1.0f / 60, victims),
                    second.traps.update(1, 1.0f / 60, victims));
        CHECK(second.digest() == first.digest());
    }
    CHECK(second.device.texturesCreated == textures);
}
TEST_CASE(
    "native trap rosters restore every authored and disarmed figure on independent stage loads",
    "[rollback][traps-local-rewind][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto* const stage = GENERATE("F1", "J4", "G2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Traps a;
    Traps b;
    a.bind(device, world.layout(), world.itemScope(), &world.collision(), 42, 1, 1);
    b.bind(device, world.layout(), world.itemScope(), &world.collision(), 42, 1, 1);
    const auto digestOf = [&](const Traps& traps) {
        NativeStateResources resources;
        test::registerStage(resources, world, device);
        return test::liveDigest(traps, resources);
    };
    Traps::LocalSnapshot authored;
    a.saveLocal(authored);
    CHECK(digestOf(b) == digestOf(a));
    const auto textures = device.texturesCreated;
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(b.trap(i).name == a.trap(i).name);
        a.disarm(i, device, world.layout(), world.itemScope(), &world.collision());
        b.disarm(i, device, world.layout(), world.itemScope(), &world.collision());
    }
    Traps::LocalSnapshot disarmed;
    a.saveLocal(disarmed);
    CHECK(digestOf(b) == digestOf(a));
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(b.trap(i).name == a.trap(i).name);
    }
    REQUIRE(a.restoreLocal(authored));
    CHECK(holds(a, authored));
    REQUIRE(a.restoreLocal(disarmed));
    CHECK(holds(a, disarmed));
    CHECK(device.texturesCreated == textures);
}
TEST_CASE("trap floor attachments rewind with native nonblocking moving support",
          "[rollback][traps-local-rewind]") {
    const auto rate = GENERATE(30, 60);
    Fixture first(true);
    Fixture second(true);
    REQUIRE(first.traps.trap(0).support.object() == 0);
    REQUIRE_FALSE(first.traps.trap(0).box.solid);
    const Vec3 initialPosition = first.traps.trap(0).figure.position();
    const auto textures = second.device.texturesCreated;
    for (s32 tick = 0; tick < 90; ++tick) {
        const f32 phase = static_cast<f32>(tick) * 0.02f;
        const auto position = glm::translate(Mat4{1}, Vec3{phase, -phase, phase}) *
                              glm::rotate(Mat4{1}, phase, Vec3{0, 1, 0});
        first.collision.setObjectTransform(0, position);
        second.collision.setObjectTransform(0, position);
        first.collision.setSolid(0, false);
        second.collision.setSolid(0, false);
        first.traps.capturePresentation();
        second.traps.capturePresentation();
        first.traps.update(60 / rate, 1.0f / static_cast<f32>(rate), {});
        second.traps.update(60 / rate, 1.0f / static_cast<f32>(rate), {});
        CHECK(second.digest() == first.digest());
        CHECK(first.traps.trap(0).support.object() == 0);
        CHECK_FALSE(first.traps.trap(0).box.solid);
    }
    CHECK(first.traps.trap(0).figure.position() != initialPosition);
    Traps::LocalSnapshot beforeDetach;
    second.traps.saveLocal(beforeDetach);
    second.collision.clear();
    second.traps.syncFloors();
    Traps::LocalSnapshot detached;
    second.traps.saveLocal(detached);
    CHECK(second.traps.trap(0).support.object() == -1);
    REQUIRE(second.traps.restoreLocal(beforeDetach));
    CHECK(holds(second.traps, beforeDetach));
    CHECK_FALSE(second.collision.loaded()); // external collision belongs to the aggregate
    second.traps.syncFloors();
    CHECK(holds(second.traps, detached));
    CHECK(second.device.texturesCreated == textures);
}
TEST_CASE("trap native values undo random cycles contact gaps disarming and first effect births",
          "[local-rewind][fixture-local-rewind][traps-local-rewind]") {
    Fixture native(true);
    Fixture replay(true);
    std::array<Traps::LocalSnapshot, 16> history;
    WorldCollision::LocalSnapshot floor;
    const auto uploads = replay.device.texturesCreated;
    const auto step = [](Fixture& fixture, s32 tick, bool wrong) {
        const auto frame = static_cast<f32>(tick);
        const Mat4 placement = glm::translate(Mat4{1}, Vec3{0, -0.005f * frame, 0});
        fixture.collision.setObjectTransform(0, placement);
        fixture.traps.capturePresentation();
        fixture.traps.setPlayerCount(wrong ? 4 : 1);
        if (wrong || tick == 200) {
            fixture.traps.disarm(0, fixture.device, fixture.layout, AssetScope(fixture.items),
                                 &fixture.collision);
        }
        if (wrong || tick == 260) {
            fixture.traps.disarm(1, fixture.device, fixture.layout, AssetScope(fixture.items),
                                 &fixture.collision);
        }
        if (tick == 120) {
            fixture.traps.stop(1);
        }
        const Vec3 first{placement * Vec4{0, 1, 0, 1}};
        const Vec3 second{placement * Vec4{20, 1, 0, 1}};
        const std::array victims{TrapVictim{first, 0.75f, 1.5f}, TrapVictim{second, 0.75f, 1.5f}};
        return fixture.traps.update(wrong ? 11 : 1, wrong ? 0.2f : 1.0f / 60, victims,
                                    !wrong && tick >= 70 && tick < 80);
    };
    usize hits = 0;
    usize wakes = 0;
    bool particles = false;
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        const auto distance = static_cast<s32>(window % 8 + 1);
        CAPTURE(window, distance);
        auto& saved = history[window % history.size()];
        replay.traps.saveLocal(saved);
        replay.collision.saveLocal(floor);
        CHECK_FALSE(native.traps.restoreLocal(saved));
        CHECK_FALSE(native.traps.equalLocal(saved, saved));
        std::vector<Traps::LocalSnapshot> nativeStates(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.traps.saveLocal(nativeStates[static_cast<usize>(i)]);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.traps.restoreLocal(saved));
        for (s32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        replay.traps.draw(replay.device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 0.6f);
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.traps.restoreLocal(saved));
        CHECK(holds(replay.traps, saved));
        for (s32 i = 0; i < distance; ++i) {
            const auto expected = step(native, tick + i, false);
            compareHits(step(replay, tick + i, false), expected);
            hits += expected.size();
            wakes += native.traps.wakes().size();
            CHECK(replay.digest() == native.digest());
            Traps::LocalSnapshot actual;
            replay.traps.saveLocal(actual);
            CHECK(replay.traps.equalLocal(actual, nativeStates[static_cast<usize>(i)]));
            particles |= native.traps.trap(0).figure.particles().field().particleCount() > 0;
        }
        tick += distance;
    }
    CHECK(hits > 0);
    CHECK(wakes > 0);
    CHECK(particles);
    CHECK(replay.traps.trap(0).disarmed);
    CHECK(replay.traps.trap(1).gone);
    CHECK(replay.device.texturesCreated == uploads);
    auto& saved = history.front();
    replay.traps.saveLocal(saved);
    REQUIRE(replay.traps.restoreLocal(saved));
    replay.traps.clear();
    CHECK_FALSE(replay.traps.restoreLocal(saved));
    CHECK_FALSE(replay.traps.equalLocal(saved, saved));
}

TEST_CASE("native trap equality detects victim cooldowns with identical trap figures",
          "[local-rewind][fixture-local-rewind][traps-local-rewind]") {
    Fixture fixture;
    Traps::LocalSnapshot initial;
    Traps::LocalSnapshot untouched;
    Traps::LocalSnapshot hit;
    Traps::LocalSnapshot actual;
    const Traps::LocalSnapshot empty{};
    for (s32 tick = 0; tick < 100 && fixture.traps.trap(0).action != 2; ++tick) {
        fixture.traps.update(1, 1.0f / 60, {});
    }
    REQUIRE(fixture.traps.trap(0).action == 2);
    fixture.traps.saveLocal(initial);
    CHECK_FALSE(fixture.traps.equalLocal(initial, empty));
    CHECK_FALSE(fixture.traps.equalLocal(empty, empty));
    const std::array far{TrapVictim{Vec3{100, 1, 0}}};
    const std::array near{TrapVictim{Vec3{0, 1, 0}}};
    REQUIRE(fixture.traps.update(0, 0, far).empty());
    fixture.traps.saveLocal(untouched);
    std::vector<Traps::Trap> figures;
    for (usize i = 0; i < fixture.traps.size(); ++i) {
        figures.push_back(fixture.traps.trap(i));
    }

    REQUIRE(fixture.traps.restoreLocal(initial));
    const auto hits = fixture.traps.update(0, 0, near);
    REQUIRE(hits.size() == 1);
    fixture.traps.saveLocal(hit);
    for (usize i = 0; i < fixture.traps.size(); ++i) {
        CHECK(fixture.traps.trap(i) == figures[i]);
    }
    CHECK_FALSE(fixture.traps.equalLocal(untouched, hit));
    CHECK_FALSE(fixture.traps.equalLocal(hit, untouched));
    REQUIRE(fixture.traps.update(0, 0, near).empty());
    REQUIRE(fixture.traps.restoreLocal(untouched));
    compareHits(fixture.traps.update(0, 0, near), hits);
    fixture.traps.saveLocal(actual);
    CHECK(fixture.traps.equalLocal(hit, actual));

    REQUIRE(fixture.traps.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr,
                               7123, 1.25f, 1.5f));
    fixture.traps.saveLocal(actual);
    CHECK_FALSE(fixture.traps.equalLocal(initial, actual));
    CHECK_FALSE(fixture.traps.equalLocal(initial, initial));
}
} // namespace
