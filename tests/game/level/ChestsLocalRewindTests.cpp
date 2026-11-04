#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/Chests.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

auto eventValues(const ChestEvent& value) {
    return std::tie(value.kind, value.chest, value.visitor, value.contents, value.gold,
                    value.explodes, value.position);
}

void checkEvents(std::span<const ChestEvent> actual, std::span<const ChestEvent> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK((eventValues(actual[index]) == eventValues(expected[index])));
    }
}

/** Whether `chests` now hold exactly the values `saved` holds. */
bool holds(const Chests& chests, const Chests::LocalSnapshot& saved) {
    Chests::LocalSnapshot now;
    chests.saveLocal(now);
    return chests.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    Chests chests;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":2,"subtype":47,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":2,"subtype":44,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":-1,"choices":[4,5]},
            {"type":1,"subtype":2,"name":"KEY","radius":1,"value":1},
            {"type":1,"subtype":1,"name":"KEY","radius":1,"value":200}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[3,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":1,"position":[20,0,0],"params":[5,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":2,"position":[40,0,0],"params":[4,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":0,"position":[60,0,0],"minPlayers":2,
             "params":[3,0,0,0,1,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"CHEST","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30,
              "tracks":[{"node":1,"flags":32,"frames":[0,7],"values":[0,2]}]},
             {"name":"OPEN","frames":0}]},
          {"name":"KEY","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(chests.bind(device, layout, AssetScope(items), nullptr));
        chests.setPlayerCount(2);
        REQUIRE(chests.size() == 4);
        REQUIRE(chests.chest(0).figure.hasFigure());
    }
};

TEST_CASE("native chest history cancels opening rewards transmutation and first preview births",
          "[local-rewind][chest-local-rewind]") {
    Fixture pool("chest-local-history");
    Chests::LocalSnapshot initial;
    pool.chests.saveLocal(initial);
    std::array<Chests::LocalSnapshot, 16> history;
    const auto step = [&](s32 tick, bool wrong) {
        pool.chests.capturePresentation();
        const s32 phase = std::min(tick / 20, 3);
        const std::array party{
            ChestVisitor{Vec3{static_cast<f32>((wrong ? 3 - phase : phase) * 20), 0, 0}, 1,
                         wrong ? 0 : (tick % 20 < 3 ? 0 : 1), tick % 7 == 0}};
        (void)pool.chests.updateXray(pool.device, AssetScope(pool.items), pool.items, party);
        if (tick == 19 || wrong) {
            (void)pool.chests.transmute(3, wrong ? 5 : 4, wrong ? 60.0f : 9.0f);
        }
        if (tick == 39) {
            (void)pool.chests.changeContents(2, 5, pool.device, pool.layout, AssetScope(pool.items),
                                             nullptr, "KEY");
        }
        return pool.chests.update(1.0f / 60, party);
    };
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window);
        REQUIRE(pool.chests.restoreLocal(initial));
        for (s32 tick = 0; tick < window; ++tick) {
            (void)step(tick, false);
        }
        auto& saved = history[static_cast<usize>(window) % history.size()];
        pool.chests.saveLocal(saved);
        const s32 distance = window % 8 + 1;
        std::vector<Chests::LocalSnapshot> nativeStates(static_cast<usize>(distance));
        std::vector<std::vector<ChestEvent>> events;
        for (s32 tick = window; tick < window + distance; ++tick) {
            events.push_back(step(tick, false));
            pool.chests.saveLocal(nativeStates[static_cast<usize>(tick - window)]);
        }
        REQUIRE(pool.chests.restoreLocal(saved));
        for (s32 tick = window; tick < window + distance; ++tick) {
            (void)step(tick, true);
        }
        REQUIRE(pool.chests.restoreLocal(saved));
        for (s32 tick = window; tick < window + distance; ++tick) {
            const auto at = static_cast<usize>(tick - window);
            checkEvents(step(tick, false), events[at]);
            Chests::LocalSnapshot actual;
            pool.chests.saveLocal(actual);
            CHECK(pool.chests.equalLocal(actual, nativeStates[at]));
        }
    }
    CHECK(pool.device.textureUpdates == 0);
    pool.chests.saveLocal(initial);
    REQUIRE(pool.chests.restoreLocal(initial));
    Chests other;
    CHECK_FALSE(other.restoreLocal(initial));
    CHECK_FALSE(other.equalLocal(initial, initial));
    pool.chests.clear();
    CHECK_FALSE(pool.chests.restoreLocal(initial));
    CHECK_FALSE(pool.chests.equalLocal(initial, initial));
}

TEST_CASE("native chest equality detects hidden pickup ownership and refusal clocks",
          "[local-rewind][chest-local-rewind]") {
    Fixture fixture("chest-local-hidden-equality");
    Chests::LocalSnapshot initial;
    Chests::LocalSnapshot changed;
    Chests::LocalSnapshot actual;
    const Chests::LocalSnapshot empty{};
    fixture.chests.saveLocal(initial);
    REQUIRE(fixture.chests.equalLocal(initial, initial));
    CHECK_FALSE(fixture.chests.equalLocal(initial, empty));
    CHECK_FALSE(fixture.chests.equalLocal(empty, empty));

    const auto first = fixture.chests.chest(0);
    fixture.chests.hold(0, 91);
    fixture.chests.saveLocal(changed);
    REQUIRE(fixture.chests.chest(0).figure == first.figure);
    REQUIRE(fixture.chests.chest(0).box == first.box);
    CHECK_FALSE(fixture.chests.equalLocal(initial, changed));
    CHECK_FALSE(fixture.chests.equalLocal(changed, initial));
    REQUIRE(fixture.chests.restoreLocal(initial));
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(initial, actual));

    // Equal model clocks and collision do not imply equal refusal cooldowns.
    REQUIRE(fixture.chests.update(0, {}).empty());
    fixture.chests.saveLocal(initial);
    const auto before = fixture.chests.chest(0);
    const std::array party{ChestVisitor{Vec3{0}, 1, 0}};
    const auto events = fixture.chests.update(0, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events.front().kind == ChestEvent::Kind::Refused);
    fixture.chests.saveLocal(changed);
    CHECK(fixture.chests.chest(0).figure == before.figure);
    CHECK(fixture.chests.chest(0).box == before.box);
    CHECK_FALSE(fixture.chests.equalLocal(initial, changed));
    REQUIRE(fixture.chests.restoreLocal(initial));
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(initial, actual));
    checkEvents(fixture.chests.update(0, party), events);
    fixture.chests.saveLocal(actual);
    CHECK(fixture.chests.equalLocal(changed, actual));

    REQUIRE(
        fixture.chests.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr));
    fixture.chests.saveLocal(actual);
    CHECK_FALSE(fixture.chests.equalLocal(initial, actual));
    CHECK_FALSE(fixture.chests.equalLocal(initial, initial));
}

TEST_CASE("native chest rewind keeps bound definitions but restores choices and their seed",
          "[local-rewind][chest-local-rewind]") {
    Fixture fixture("chest-local-bound-definitions");
    const u32 seed = fixture.chests.simulation().seed;
    Chests::LocalSnapshot saved;
    fixture.chests.saveLocal(saved);
    const std::array party{ChestVisitor{Vec3{0}, 1, 1}};
    const auto expectedEvents = fixture.chests.update(1.0f / 60, party);
    REQUIRE(expectedEvents.size() == 1);
    REQUIRE(expectedEvents.front().kind == ChestEvent::Kind::Unlocked);
    REQUIRE(expectedEvents.front().contents >= 4);
    REQUIRE(expectedEvents.front().contents <= 5);
    Chests::LocalSnapshot expected;
    fixture.chests.saveLocal(expected);
    REQUIRE(fixture.chests.simulation().seed != seed);

    REQUIRE(fixture.chests.restoreLocal(saved));
    REQUIRE(fixture.chests.transmute(0, 5, 10));
    (void)fixture.chests.update(1.0f / 60, party);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(fixture.chests.restoreLocal(saved));
    CHECK(holds(fixture.chests, saved));
    checkEvents(fixture.chests.update(1.0f / 60, party), expectedEvents);
    CHECK(holds(fixture.chests, expected));
    CHECK(fixture.device.texturesCreated == uploads);

    // Rebinding even the same definition table starts a new owner epoch. The
    // table stays owned by the fixture, never by an old native history slot.
    REQUIRE(
        fixture.chests.bind(fixture.device, fixture.layout, AssetScope(fixture.items), nullptr));
    fixture.chests.setPlayerCount(2);
    CHECK_FALSE(fixture.chests.restoreLocal(saved));
    checkEvents(fixture.chests.update(1.0f / 60, party), expectedEvents);
    fixture.chests.saveLocal(saved);
    fixture.chests.clear();
    CHECK_FALSE(fixture.chests.restoreLocal(saved));
}

TEST_CASE("unbound container art and previews retain exact moving-floor presentation state",
          "[chest-local-rewind]") {
    const auto directory = test::scratchDirectory("chests-checkpoint-empty-rider");
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"ROOT","position":[100,10,50],"child":1},
      {"name":"LIFT","position":[0,0,0],"flags":4100}],
      "itemInfos":[{"type":2,"subtype":46,"name":"MISSING","radius":1,"height":2}],
      "itemInstances":[{"info":0,"position":[101,10,50],"params":[255,255,0,0,1,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    WorldCollision collision;
    collision.build({{{0, 1, 0}, {Vec3{-4, 0, -4}, Vec3{4, 0, -4}, Vec3{0, 0, 4}}, 1, 4100}});
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{100, 20, 50}));
    test::FakeRenderDevice device;
    ItemArchive empty;
    Chests chests;
    REQUIRE(chests.bind(device, layout, AssetScope(empty), &collision));
    REQUIRE(chests.chest(0).floor);
    chests.capturePresentation();
    Chests::LocalSnapshot before;
    chests.saveLocal(before);
    REQUIRE_FALSE(chests.chest(0).figure.hasFigure());
    REQUIRE_FALSE(chests.chest(0).preview.hasFigure());
    // The preview rides the chest at the overlay's 0.65 scale, which its placement carries.
    CHECK(chests.chest(0).preview.transform() ==
          glm::scale(chests.chest(0).figure.transform(), Vec3{0.65f}));
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{100, -10, 50}) * glm::rotate(Mat4{1}, 0.7f, Vec3{0, 1, 0});
    collision.setObjectTransform(1, moved);
    chests.syncFloors();
    chests.capturePresentation();
    Chests::LocalSnapshot movedState;
    chests.saveLocal(movedState);
    CHECK_FALSE(chests.equalLocal(movedState, before));
    REQUIRE(chests.restoreLocal(before));
    CHECK(holds(chests, before));
    // The support owner is external: undoing a chest must not move that floor.
    CHECK(collision.objectTransform(1) == moved);
    chests.syncFloors();
    chests.capturePresentation();
    CHECK(holds(chests, movedState));
    CHECK(device.texturesCreated == 0);
}

TEST_CASE("native campaign containers retain full authored roster and preloaded figures",
          "[chest-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto* const name = GENERATE("G1", "G3", "G4");
    CAPTURE(name);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    Chests chests;
    REQUIRE(chests.bind(device, world.layout(), world.itemScope(), &world.collision()));
    Chests::LocalSnapshot before;
    chests.saveLocal(before);
    const auto uploads = device.texturesCreated;
    chests.setPlayerCount(4);
    chests.capturePresentation();
    chests.update(1.0f / 30, {});
    REQUIRE(chests.restoreLocal(before));
    CHECK(holds(chests, before));
    CHECK(device.texturesCreated == uploads);
}
} // namespace
