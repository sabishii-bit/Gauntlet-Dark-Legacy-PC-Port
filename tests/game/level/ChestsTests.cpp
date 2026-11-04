#include <algorithm>
#include <array>
#include <set>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/io/File.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "game/level/Chests.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level with one random (silver) chest choosing among three gold records, a chest holding
 * junk outright, and a realm archive lending the gilded chest trees. */
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    ItemArchive realm;
    Chests chests;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":2,"subtype":48,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":-1,"choices":[2,3,4]},
            {"type":1,"subtype":1,"name":"JUNK","radius":1,"value":5},
            {"type":1,"subtype":1,"name":"TREAS_SILVER","radius":1,"value":100},
            {"type":1,"subtype":1,"name":"TREAS_GOLD","radius":1,"value":200},
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[1,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":5,"position":[20,0,0],"params":[2,0,0,0,1,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"CHEST","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30},
             {"name":"OPEN","frames":0}]},
          {"name":"JUNK","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]},
          {"name":"TREAS_SILVER","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]},
          {"name":"TREAS_GOLD","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":8,"frameRate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        // The realm's archive is where the gilded chests live, after the level's own.
        const auto realmDirectory = test::sampleLevel(std::string(name) + "-realm");
        writeTextFile(realmDirectory / "animations.json", R"({"trees":[
          {"name":"CHESTG1","nodes":[
             {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
             {"name":"NULL1","parent":0,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"frameRate":30},
             {"name":"ACTIVE","frames":8,"frameRate":30},
             {"name":"OPEN","frames":0}]}]})");
        REQUIRE(realm.load(realmDirectory));
        REQUIRE(chests.bind(device, layout, AssetScope(items, &realm), nullptr));
        chests.setPlayerCount(1);
        REQUIRE(chests.size() == 2);
    }
    AssetScope scope() { return {items, &realm}; }
};

TEST_CASE("a random chest cycles through its choices with the item clock until it is opened",
          "[game][level][chests][random-chest]") {
    Fixture f("chests-random-cycle");
    constexpr f32 kFrame = 1.0f / 30.0f;
    const std::array watcher{ChestVisitor{Vec3{0}, 1, 0, true}};
    // The X-ray overlay follows the pick: as the clock runs, it shows each choice in turn.
    std::set<const TreeInfo*> shown;
    s32 changes = 0;
    const TreeInfo* last = nullptr;
    for (s32 frame = 0; frame < 32 * 4; ++frame) {
        f.chests.update(kFrame, {});
        f.chests.updateXray(f.device, f.scope(), f.items, watcher);
        const TreeInfo* preview = f.chests.chest(0).preview.tree();
        REQUIRE(preview != nullptr);
        if (preview != last) {
            ++changes;
            last = preview;
        }
        shown.insert(preview);
    }
    CHECK(shown.size() == 3);
    CHECK(changes >= 3);
    // Half a frame leaves its fraction for the next update: no tick is lost or doubled.
    Chests::LocalSnapshot before;
    f.chests.saveLocal(before);
    f.chests.update(kFrame * 0.5f, {});
    f.chests.update(kFrame * 0.5f, {});
    Chests::LocalSnapshot after;
    f.chests.saveLocal(after);
    CHECK_FALSE(f.chests.equalLocal(before, after));
}

TEST_CASE("gilding a chest's junk dresses it in the realm's gold chest and refreshes the X-ray",
          "[game][level][chests][magic-perks]") {
    Fixture f("chests-gilded");
    const std::array watcher{ChestVisitor{Vec3{20, 0, 0}, 1, 0, true}};
    f.chests.update(0.0f, {});
    REQUIRE(f.chests.updateXray(f.device, f.scope(), f.items, watcher) == 1);
    REQUIRE(f.chests.chest(1).preview.tree() != nullptr);
    CHECK(f.chests.chest(1).preview.tree()->name == "JUNK");
    REQUIRE(f.chests.chest(1).figure.tree() != nullptr);
    CHECK(f.chests.chest(1).figure.tree()->name == "CHEST");
    REQUIRE(f.chests.changeContents(1, 3, f.device, f.layout, f.scope(), nullptr, "CHESTG1"));
    CHECK(f.chests.chest(1).contents == 3);
    REQUIRE(f.chests.chest(1).figure.tree() != nullptr);
    CHECK(f.chests.chest(1).figure.tree()->name == "CHESTG1");
    // The overlay shows the silver the moment it is looked at again.
    f.chests.updateXray(f.device, f.scope(), f.items, watcher);
    REQUIRE(f.chests.chest(1).preview.tree() != nullptr);
    CHECK(f.chests.chest(1).preview.tree()->name == "TREAS_SILVER");
}

TEST_CASE("the developer console decides whether a shut chest wants a key",
          "[game][level][chests][dev-console]") {
    Fixture f("chests-console-lock");
    REQUIRE(f.chests.chest(0).locked);
    const std::array keyless{ChestVisitor{Vec3{0}, 1, 0}};
    f.chests.update(0, keyless);
    CHECK(f.chests.chest(0).state == Chests::kShut); // refused without a key
    REQUIRE(f.chests.setLocked(0, false));
    CHECK_FALSE(f.chests.chest(0).locked);
    f.chests.update(3, keyless); // the refusal's pause is over: it opens without one
    CHECK(f.chests.chest(0).state == Chests::kOpening);
    CHECK_FALSE(f.chests.setLocked(0, true)); // open already: a key no longer matters
    REQUIRE(f.chests.setLocked(1, true));
    CHECK(f.chests.chest(1).locked);
    REQUIRE(f.chests.setLocked(1, false));
    CHECK_FALSE(f.chests.chest(1).locked);
    CHECK_FALSE(f.chests.setLocked(7, true));
}

/** A level of four locked chests: one picking a key from a list, a chest of gold, a trapped
 * chest, and the first kind again for parties of two. */
struct KeyedChests {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    Chests chests;

    explicit KeyedChests(std::string_view name) {
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
    /** X-ray wearers by the first and the last chest show what both hold. */
    void reveal() {
        const std::array party{ChestVisitor{Vec3{0}, 1, 0, true},
                               ChestVisitor{Vec3{60, 0, 0}, 1, 0, true}};
        REQUIRE(chests.updateXray(device, AssetScope(items), items, party) == 2);
        REQUIRE(chests.chest(0).preview.hasFigure());
        REQUIRE(chests.chest(3).preview.hasFigure());
    }
};

TEST_CASE("a revealed chest's preview plays the pickup's cycle instead of holding its first frame",
          "[chests][xray]") {
    KeyedChests pool("chest-preview-cycle");
    pool.reveal();
    const auto placed = pool.chests.chest(0).preview.checkpoint();
    REQUIRE(placed);
    CHECK(placed->poseFrame == 0);
    for (s32 frame = 0; frame < 6; ++frame) {
        (void)pool.chests.update(1.0f / 60, {});
    }
    const auto later = pool.chests.chest(0).preview.checkpoint();
    REQUIRE(later);
    CHECK(later->poseFrame > 0);
    CHECK(pool.chests.chest(0).revealed);
}

TEST_CASE("visited chests refuse open pay gold blow up and keep their opened pose",
          "[game][level][chests]") {
    KeyedChests host("chests-visited");
    host.reveal();
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto hostUploads = host.device.texturesCreated;
    bool refused = false;
    bool unlocked = false;
    bool opened = false;
    bool collected = false;
    bool exploded = false;
    bool emptyOpenPose = false;
    for (s32 tick = 0; tick < 180; ++tick) {
        const s32 phase = std::min(tick / 40, 3);
        const std::array party{
            ChestVisitor{Vec3{static_cast<f32>(phase * 20), 0, 0}, 1, tick % 40 < 4 ? 0 : 1}};
        if (tick == 120) {
            REQUIRE(host.chests.transmute(3, 4, 31));
        }
        host.chests.capturePresentation();
        const auto events = host.chests.update(seconds, party);
        for (const auto& event : events) {
            refused |= event.kind == ChestEvent::Kind::Refused;
            unlocked |= event.kind == ChestEvent::Kind::Unlocked;
            opened |= event.kind == ChestEvent::Kind::Opened;
            collected |= event.kind == ChestEvent::Kind::Collected;
            exploded |= event.explodes;
        }
        const auto figure = host.chests.chest(0).figure.checkpoint();
        emptyOpenPose |= figure && figure->holdPose;
    }
    CHECK(refused);
    CHECK(unlocked);
    CHECK(opened);
    CHECK(collected);
    CHECK(exploded);
    CHECK(emptyOpenPose);
    CHECK(host.chests.simulation().seed > 0);
    CHECK(host.device.texturesCreated == hostUploads);

    Chests::LocalSnapshot before;
    host.chests.saveLocal(before);
    host.chests.hold(0, 91);
    host.chests.remove(2);
    host.chests.setPlayerCount(1);
    REQUIRE(host.chests.restoreLocal(before));
    Chests::LocalSnapshot restored;
    host.chests.saveLocal(restored);
    CHECK(host.chests.equalLocal(restored, before));
}

} // namespace
