#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/ItemFigure.h"
#include "game/world/LevelWorld.h"
#include "game/world/LockedGates.h"
#include "game/world/Traps.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

TEST_CASE("Temple chests ride their native switch-driven platforms",
          "[game][world][fixtures][chest-platform][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Chests chests;
    REQUIRE(chests.bind(device, world.layout(), world.items(), &world.collision(),
                        &world.realmItems()));
    std::vector<Vec3> before;
    for (usize i = 0; i < chests.size(); ++i) {
        before.push_back(chests.chest(i).figure.position());
    }
    for (usize i = 0; i < world.triggers().size(); ++i) {
        world.activateTrigger(world.triggers().trigger(i).id, false);
    }
    usize descending = 0;
    for (s32 frame = 0; frame < 600; ++frame) {
        world.update(1.0f / 60);
        world.updateTriggers(1.0f / 60, {});
        chests.update(1.0f / 60, {});
        for (usize i = 0; i < chests.size(); ++i) {
            const auto& chest = chests.chest(i);
            if (!chest.floor) {
                continue;
            }
            const auto support = world.collision().objectTransform(chest.floor->object);
            REQUIRE(support);
            const Vec3 expected = Vec3{(*support * chest.floor->local)[3]};
            CHECK(glm::distance(chest.figure.position(), expected) < 0.001f);
            CHECK(glm::distance(chest.box.centre, expected) < 0.001f);
            if (frame == 599 && expected.y < before[i].y - 1.0f) {
                ++descending;
            }
        }
    }
    REQUIRE(descending > 0);
}

/** A level with a locked chest of potions-or-keys at the origin, a chest of gold at x 20, a
 * trapped one at x 40, a barrel at x 60, a gate across x 80 and spikes at x 100. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 2, "subtype": 46, "name": "CHEST", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 1, "subtype": 2, "name": "KEY", "radius": 0.5, "height": 2, "value": 1},
    {"type": 1, "subtype": 4, "name": "POT_RED", "radius": 0.5, "height": 2, "value": 1,
     "properties": 1},
    {"type": -1, "subtype": 2, "name": "", "choices": [1, 2]},
    {"type": 2, "subtype": 47, "name": "CHESTG0", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 1, "subtype": 1, "name": "TREAS_GOLD", "radius": 1.25, "height": 2, "value": 200},
    {"type": 2, "subtype": 44, "name": "CHESTEXP", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 2, "subtype": 43, "name": "BAROBJ", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 518},
    {"type": 7, "subtype": 3, "name": "GATED", "radius": 3.7, "height": 5, "xSize": 3.5,
     "zSize": 1, "activeType": 70},
    {"type": 8, "subtype": 0, "name": "SPIKES", "radius": 3.5, "height": 5, "xSize": 2,
     "zSize": 0.4, "value": 20, "activeType": 5, "activeOff": -40},
    {"type": 10, "subtype": 44, "name": "BAREXP", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6},
    {"type": 10, "subtype": 45, "name": "BARPOI", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6},
    {"type": 10, "subtype": 43, "name": "BAROBJ", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 4, "minPlayers": 1, "position": [20, 0, 0], "rotation": [0, 0, 0],
     "params": [5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 6, "minPlayers": 2, "position": [40, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 7, "minPlayers": 1, "position": [60, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 8, "minPlayers": 1, "position": [80, 0, 0], "rotation": [0, 1.5707964, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 9, "minPlayers": 1, "position": [100, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 10, "minPlayers": 1, "position": [200, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 11, "minPlayers": 1, "position": [203, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 12, "minPlayers": 3, "position": [220, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}]
})");
    return dir;
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items; ///< left unloaded: everything works unseen

    explicit Fixture(std::string_view name) { REQUIRE(layout.load(sampleLevel(name))); }
};

TEST_CASE("chests follow descending and rotating platforms with their obstacle and preview",
          "[game][world][fixtures][chest-platform]") {
    const auto dir = test::scratchDirectory("chest-platform");
    writeTextFile(dir / "world.json", R"({"objects":[
        {"name":"ROOT","position":[100,10,50],"child":1},
        {"name":"LIFT","position":[0,0,0],"flags":4100}],
        "itemInfos":[{"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2},
                     {"type":1,"subtype":4,"name":"POT_RED","radius":1}],
        "itemInstances":[{"info":0,"position":[101,10,50],"minPlayers":1,
                          "params":[1,0,0,0,1,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldCollision collision;
    collision.build({{{0, 1, 0}, {Vec3{-4, 0, -4}, Vec3{4, 0, -4}, Vec3{0, 0, 4}}, 1, 4100}});
    collision.setMovingObjects(std::array<s32, 1>{1});
    const Mat4 initial = glm::translate(Mat4{1}, Vec3{100, 20, 50});
    collision.setObjectTransform(1, initial);
    test::FakeRenderDevice device;
    ItemArchive art;
    Chests chests;
    REQUIRE(chests.bind(device, layout, art, &collision));
    REQUIRE(chests.chest(0).floor);
    CHECK(chests.chest(0).figure.position().y == Approx(20.1f));
    CHECK(collision.objectTransform(1) == initial);
    const Mat4 local = chests.chest(0).floor->local;
    const Mat4 moved =
        glm::translate(Mat4{1}, Vec3{100, -10, 50}) * glm::rotate(Mat4{1}, 0.5f, Vec3{0, 1, 0});
    collision.setObjectTransform(1, moved);
    chests.syncFloors();
    const Mat4 expected = moved * local;
    CHECK(chests.chest(0).figure.transform() == expected);
    CHECK(chests.chest(0).preview.transform() == expected);
    CHECK(chests.obstacles()[0].centre == Vec3{expected[3]});
    CHECK(chests.obstacles()[0].yaw == Approx(0.5f));
    // Opening remains possible at the new position, never at the abandoned one.
    const std::array oldVisitor{ChestVisitor{Vec3{101, 20, 50}, 1, 1}};
    CHECK(chests.update(0, oldVisitor).empty());
    const std::array newVisitor{ChestVisitor{Vec3{expected[3]}, 1, 1}};
    const auto events = chests.update(0, newVisitor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].position == Vec3{expected[3]});
    CHECK(events[0].contents == 1);
    chests.hold(0, 42);
    collision.setObjectTransform(1, initial);
    chests.update(0, {});
    CHECK(chests.chest(0).figure.transform() == initial * local);
    CHECK(chests.chest(0).held == 42);
    for (s32 i = 0; i < 100; ++i) {
        chests.syncFloors();
    }
    CHECK(chests.chest(0).figure.transform() == initial * local);
}

TEST_CASE("X-Ray reveals the nearest closed chest without spending keys or changing its loot",
          "[game][world][fixtures][xray]") {
    Fixture f("fixtures-xray");
    Chests chests;
    REQUIRE(chests.bind(f.device, f.layout, f.items, nullptr));
    chests.setPlayerCount(2);
    std::array party{ChestVisitor{Vec3{3, 0, 0}, 0.75f, 0, true}};
    CHECK(chests.updateXray(f.device, f.items, f.items, 0, party) == 1);
    CHECK(chests.chest(0).revealed);
    CHECK(chests.chest(0).state == Chests::kShut);
    CHECK(chests.chest(0).held == -1);
    const auto preview = chests.chest(0).previewContents;
    CHECK(preview == 1);
    CHECK(chests.updateXray(f.device, f.items, f.items, 0, party) == 0);
    party[0].xray = false;
    CHECK(chests.updateXray(f.device, f.items, f.items, 0, party) == 0);
    CHECK_FALSE(chests.chest(0).revealed);
    party[0].xray = true;
    party[0].position = Vec3{10, 0, 0};
    chests.updateXray(f.device, f.items, f.items, 0, party);
    CHECK_FALSE(chests.chest(0).revealed); // strict ten-unit reach
    party[0].position = Vec3{21.9f, 0, 0};
    CHECK(chests.updateXray(f.device, f.items, f.items, 0, party) == 1);
    CHECK(chests.chest(1).revealed);
    party[0].position = Vec3{41.9f, 0, 0};
    chests.updateXray(f.device, f.items, f.items, 0, party);
    CHECK(chests.chest(2).revealed); // a trap must be visible before spending a key on it
    party[0].position = Vec3{1.9f, 0, 0};
    party[0].keys = 1;
    const auto opened = chests.update(0, party);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].contents == preview);
    chests.updateXray(f.device, f.items, f.items, 0, party);
    CHECK_FALSE(chests.chest(0).revealed);
}

TEST_CASE("a box pushes a body out by its nearest side and knows what is against it",
          "[game][world][fixtures]") {
    Obstacle box;
    box.centre = Vec3{10.0f, 0.0f, 5.0f};
    box.halfAcross = 2.0f;
    box.halfAlong = 1.0f;
    box.height = 2.0f;
    // Clear of it, a body stays; against it, it is put a radius off the side.
    REQUIRE(box.pushOut(Vec3{20.0f, 0.0f, 5.0f}, 0.75f) == Vec3{20.0f, 0.0f, 5.0f});
    const Vec3 side = box.pushOut(Vec3{12.2f, 0.0f, 5.0f}, 0.75f);
    REQUIRE(side.x == Approx(12.75f));
    REQUIRE(side.z == Approx(5.0f));
    const Vec3 inside = box.pushOut(Vec3{10.0f, 0.0f, 5.6f}, 0.75f);
    REQUIRE(inside.z == Approx(6.75f)); // out by the nearer, long side
    REQUIRE(inside.x == Approx(10.0f));
    // Over it, it is not in the way.
    REQUIRE(box.pushOut(Vec3{10.0f, 9.0f, 5.0f}, 0.75f).y == 9.0f);
    REQUIRE(box.touchedBy(Vec3{12.9f, 0.0f, 5.0f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{13.2f, 0.0f, 5.0f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{12.9f, 9.0f, 5.0f}, 0.75f));
    // Turned a quarter, its long way lies along z.
    box.yaw = 1.5707964f;
    REQUIRE(box.touchedBy(Vec3{10.0f, 0.0f, 7.5f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{12.5f, 0.0f, 5.0f}, 0.75f));
    box.solid = false;
    REQUIRE(box.pushOut(Vec3{10.0f, 0.0f, 5.0f}, 0.75f) == Vec3{10.0f, 0.0f, 5.0f});
    // With pitch/roll at pi, the world-axis yaw's sign is reversed as well.
    const Mat4 flipped = itemPlacement(Vec3{1.0f, 2.0f, 3.0f}, Vec3{kPi, 0.5f, -kPi});
    const Mat4 turned = itemPlacement(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, kPi - 0.5f, 0.0f});
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            REQUIRE(flipped[column][row] == Approx(turned[column][row]).margin(1e-5));
        }
    }
    REQUIRE(shownToParty(1, 1));
    REQUIRE_FALSE(shownToParty(3, 2));
    REQUIRE(shownToParty(12, 2)); // exactly two
    REQUIRE_FALSE(shownToParty(12, 3));
}

TEST_CASE("item placement applies authored world-axis rotations without reversing wall facings",
          "[game][world][fixtures][item-orientation]") {
    const std::array rotations{Vec3{0, kPi / 2, 0}, Vec3{0, -kPi / 2, 0}, Vec3{kPi, 0.5f, -kPi},
                               Vec3{0.3f, -0.7f, 0.2f}};
    const Vec3 position{12, 4, -8};
    // AddItemInstList calls WPitchMat3, WYawMat3, WRollMat3 in that order.
    // Transform each basis vector independently using those routines' scalar equations.
    for (const Vec3& angles : rotations) {
        const Mat4 actual = itemPlacement(position, angles);
        for (s32 axis = 0; axis < 3; ++axis) {
            Vec3 basis{0};
            basis[axis] = 1;
            const Vec3 pitched{basis.x, std::cos(angles.x) * basis.y - std::sin(angles.x) * basis.z,
                               std::cos(angles.x) * basis.z + std::sin(angles.x) * basis.y};
            const Vec3 yawed{std::cos(angles.y) * pitched.x - std::sin(angles.y) * pitched.z,
                             pitched.y,
                             std::cos(angles.y) * pitched.z + std::sin(angles.y) * pitched.x};
            const Vec3 expected{std::cos(angles.z) * yawed.x - std::sin(angles.z) * yawed.y,
                                std::cos(angles.z) * yawed.y + std::sin(angles.z) * yawed.x,
                                yawed.z};
            CHECK(glm::distance(Vec3{actual[axis]}, expected) < 0.00001f);
        }
        CHECK(Vec3{actual[3]} == position);
        ItemFigure figure;
        ItemArchive noArt;
        ItemInstance instance;
        instance.position = position;
        instance.rotation = angles;
        test::FakeRenderDevice device;
        figure.place(device, noArt, "absent", instance, nullptr);
        ItemInfo info;
        info.xSize = 3;
        info.zSize = 1;
        CHECK(figure.obstacle(info).yaw == Approx(std::atan2(actual[2].x, actual[2].z)));
    }
    CHECK(Vec3{itemPlacement(Vec3{0}, Vec3{0, kPi / 2, 0})[2]}.x == Approx(-1));
}

TEST_CASE("triangle-list gate floors do not consume keys but upright barriers still do",
          "[game][world][fixtures][temple-inventory]") {
    const auto directory = test::scratchDirectory("gate-surface-contact");
    writeTextFile(directory / "world.json", R"({
      "objects": [{"name":"ROOT", "position":[0,0,0]}],
      "itemInfos": [{"type":7,"name":"GATE","collisionType":4,"radius":5,"height":3}],
      "itemInstances": [
        {"info":0,"position":[0,0,0],"collision":[
          {"normal":[0,1,0],"vertices":[[-5,0,-5],[5,0,-5],[0,0,5]]}]},
        {"info":0,"position":[20,0,0],"rotation":[1.5707964,0,0],"collision":[
          {"normal":[0,1,0],"vertices":[[-5,0,-5],[5,0,-5],[0,0,5]]}]}
      ]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    ItemArchive items;
    LockedGates gates;
    REQUIRE(gates.bind(device, layout, items, nullptr));
    REQUIRE(gates.size() == 2);
    CHECK_FALSE(gates.gate(0).blocksPassage);
    CHECK(gates.gate(1).blocksPassage);
    REQUIRE(gates.obstacles().size() == 1);
    std::array visitors{ChestVisitor{Vec3{0}, 0.75f, 1}};
    CHECK(gates.update(1, 1.0f / 60, visitors).empty());
    visitors[0].position.x = 20;
    visitors[0].keys = 0;
    auto events = gates.update(1, 1.0f / 60, visitors);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GateEvent::Kind::Refused);
    visitors[0].keys = 1;
    events = gates.update(1, 1.0f / 60, visitors);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GateEvent::Kind::Unlocked);
}

TEST_CASE("a chest's contents are its record, or the pick from a list by the item's place",
          "[game][world][fixtures]") {
    const Fixture f("fixtures-contents");
    u32 seed = 0;
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 5, 0, seed) == 5);
    REQUIRE(seed == 0U);
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 0, seed) == 1); // (0 + 0) % 2
    REQUIRE(seed == static_cast<u32>(Chests::kSeedStep));
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 1, seed) == 1); // (13 + 1) % 2
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 0, seed) == 2); // (27 + 0) % 2
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), -1, 0, seed) == -1);
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 99, 0, seed) == -1);
}

TEST_CASE("a locked chest wants a key, opens, and gives up what it held",
          "[game][world][fixtures]") {
    Fixture f("fixtures-chests");
    Chests chests;
    REQUIRE(chests.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(chests.size() == 3); // the barrel is not a chest
    chests.setPlayerCount(1);
    REQUIRE(chests.chest(0).locked);
    REQUIRE(chests.chest(0).box.halfAcross == 1.2f);
    REQUIRE_FALSE(chests.chest(2).shown); // the trapped one is for two players
    REQUIRE(chests.obstacles().size() == 2);

    std::array<ChestVisitor, 1> party{ChestVisitor{Vec3{10.0f, 0.0f, 0.0f}, 0.75f, 0}};
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // nobody near
    party[0].position = Vec3{1.9f, 0.0f, 0.0f};
    std::vector<ChestEvent> events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Refused);
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // not told again at once
    party[0].keys = 1;
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Unlocked);
    REQUIRE(events[0].chest == 0);
    REQUIRE(chests.chest(0).state == Chests::kOpening);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Opened);
    REQUIRE(events[0].contents == 1); // the list's first pick: a key
    REQUIRE(events[0].gold == 0);
    REQUIRE(chests.chest(0).state == Chests::kOpen);
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // open, it has no more to give
    // What came out lies in it, reached by touching it; emptied, the chest goes.
    REQUIRE(chests.holdingTouchedBy(party[0]) == -1);
    chests.hold(0, 7);
    REQUIRE(chests.holdingTouchedBy(party[0]) == 0);
    REQUIRE(chests.holdingTouchedBy(ChestVisitor{Vec3{10.0f, 0.0f, 0.0f}, 0.75f, 0}) == -1);
    chests.remove(0);
    REQUIRE(chests.chest(0).gone);
    REQUIRE(chests.holdingTouchedBy(party[0]) == -1);
    REQUIRE(chests.obstacles().size() == 1);

    // Gold stays in the opened chest until a player touches it.
    party[0].position = Vec3{21.9f, 0.0f, 0.0f};
    chests.update(1.0f / 30.0f, party);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Opened);
    REQUIRE(events[0].gold == 0);
    REQUIRE(events[0].contents == -1);
    REQUIRE(chests.update(1.0f, {}).empty());
    REQUIRE_FALSE(chests.chest(1).gone);
    const std::array otherVisitor{ChestVisitor{Vec3{100, 0, 0}, 0.75f, 0}, party[0]};
    events = chests.update(0, otherVisitor);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Collected);
    REQUIRE(events[0].visitor == 1);
    REQUIRE(events[0].gold == 200);
    REQUIRE(chests.update(Chests::kCollectedSeconds * 0.5f, otherVisitor).empty());
    REQUIRE_FALSE(chests.chest(1).gone);
    REQUIRE(chests.update(Chests::kCollectedSeconds * 0.5f, otherVisitor).empty());
    REQUIRE(chests.chest(1).gone);
    chests.setPlayerCount(2);
    party[0].position = Vec3{41.9f, 0.0f, 0.0f};
    chests.update(1.0f / 30.0f, party);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].explodes);
    REQUIRE(events[0].contents == -1);
}

TEST_CASE("a gate bars the way until a key is spent on it", "[game][world][fixtures]") {
    Fixture f("fixtures-gates");
    LockedGates gates;
    REQUIRE(gates.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(gates.size() == 1);
    gates.setPlayerCount(1);
    REQUIRE(gates.obstacles().size() == 1);
    // Turned a quarter, it lies along z: someone beside it on x is against it.
    std::array<ChestVisitor, 1> party{ChestVisitor{Vec3{81.8f, 0.0f, 2.0f}, 0.75f, 0}};
    std::vector<GateEvent> events = gates.update(2, 1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == GateEvent::Kind::Refused);
    party[0].keys = 2;
    // Backing away from it, a key is neither spent nor asked for (ItemTouch's door case).
    const Vec3 away = party[0].position - gates.gate(0).figure.position();
    party[0].step = glm::normalize(Vec3{away.x, 0.0f, away.z}) * 0.2f;
    REQUIRE(gates.update(2, 1.0f / 30.0f, party).empty());
    REQUIRE(gates.gate(0).state == LockedGates::kShut);
    party[0].step = Vec3{0.0f};
    events = gates.update(2, 1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == GateEvent::Kind::Unlocked);
    REQUIRE(gates.gate(0).state != LockedGates::kShut);
    for (s32 i = 0; i < 40; ++i) {
        REQUIRE(gates.update(2, 1.0f / 30.0f, party).empty());
    }
    REQUIRE(gates.gate(0).state == LockedGates::kOpen);
    REQUIRE(gates.obstacles().empty());
}

TEST_CASE("a trap rests, comes out to hurt whoever is in it, and rests again",
          "[game][world][fixtures]") {
    Fixture f("fixtures-traps");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7));
    REQUIRE(traps.size() == 1);
    traps.setPlayerCount(1);
    REQUIRE(traps.trap(0).damage == 20.0f);
    REQUIRE_FALSE(traps.armed(0));
    // Its rest is somewhere from forty to a hundred and twenty ticks.
    REQUIRE(traps.trap(0).ticksLeft >= 40);
    REQUIRE(traps.trap(0).ticksLeft < 120);
    const std::array<TrapVictim, 2> party{TrapVictim{Vec3{100.0f, 0.0f, 0.0f}, 0.75f},
                                          TrapVictim{Vec3{120.0f, 0.0f, 0.0f}, 0.75f}};
    s32 hits = 0;
    s32 armedFrames = 0;
    bool restedAgain = false;
    s32 wakes = 0;
    for (s32 i = 0; i < 400; ++i) {
        const bool wasArmed = traps.armed(0);
        const std::vector<TrapHit> caught = traps.update(2, 1.0f / 30.0f, party);
        CHECK(traps.wakes().size() == (!wasArmed && traps.armed(0) ? 1 : 0));
        if (!traps.wakes().empty()) {
            CHECK(traps.wakes().front() == 0);
            ++wakes;
        }
        for (const TrapHit& hit : caught) {
            REQUIRE(hit.victim == 0); // the one standing in it
            REQUIRE(hit.damage == 20.0f);
            REQUIRE(traps.armed(0));
            ++hits;
        }
        armedFrames += traps.armed(0) ? 1 : 0;
        restedAgain = restedAgain || (armedFrames > 0 && !traps.armed(0));
    }
    REQUIRE(hits >= 1);
    REQUIRE(armedFrames > 0);
    REQUIRE(restedAgain);
    REQUIRE(hits <= armedFrames); // no oftener than its gap allows
    CHECK(wakes > 1);
    traps.update(100, 1, party, true);
    CHECK(traps.wakes().empty());
    traps.clear();
    CHECK(traps.wakes().empty());
}

TEST_CASE("traps prefer level-specific figures and fall back to the realm archive",
          "[game][world][fixtures][boss-stage]") {
    Fixture f("fixtures-trap-archives");
    const auto directory = test::scratchDirectory("fixtures-trap-figure");
    std::filesystem::create_directories(directory / "models");
    std::filesystem::create_directories(directory / "textures");
    writeTextFile(directory / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(
        directory / "objects.json",
        R"({"objects":[{"index":0,"name":"SPIKESBODY","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(directory / "textures/skin.png", test::kTinyPng);
    writeTextFile(
        directory / "textures.json",
        R"({"bitmaps":[{"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(
        directory / "animations.json",
        R"({"trees":[{"name":"SPIKES","nodes":[{"name":"BODY","object":"SPIKESBODY","parent":-1,"position":[0,0,0]}],
                    "sequences":[{"name":"OFF","frames":1},{"name":"ON","frames":20}]}]})");
    test::convertModelFixture(directory);
    REQUIRE(f.items.load(directory));
    ItemArchive missing;
    Traps traps;
    SECTION("realm archive supplies missing boss-level figures") {
        REQUIRE(traps.bind(f.device, f.layout, missing, nullptr, 1, 1, 1, &f.items));
    }
    SECTION("level-specific figures retain precedence") {
        REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 1, 1, 1, &missing));
    }
    REQUIRE(traps.size() == 1);
    REQUIRE(traps.trap(0).figure.hasFigure());
    REQUIRE(traps.trap(0).figure.sequenceCount() > 1);
    REQUIRE(traps.trap(0).figure.ticksOf(1) > 1);
}

TEST_CASE("a level scales how fast its traps cycle and how much they hurt",
          "[game][world][fixtures]") {
    Fixture f("fixtures-trap-scales");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7, 2.0f, 0.5f));
    REQUIRE(traps.trap(0).damage == 10.0f);
    // Twice the forty to a hundred and twenty ticks it rests unscaled.
    REQUIRE(traps.trap(0).ticksLeft >= 80);
    REQUIRE(traps.trap(0).ticksLeft < 240);
    REQUIRE(traps.trap(0).subtype == Traps::kSpikes);
    traps.setPlayerCount(1);
    // Caught, a victim is left alone until the sequence it was caught in has run out twice.
    const std::array<TrapVictim, 1> party{TrapVictim{Vec3{100.0f, 0.0f, 0.0f}, 0.75f}};
    std::vector<TrapHit> caught;
    for (s32 i = 0; i < 400 && caught.empty(); ++i) {
        caught = traps.update(2, 1.0f / 30.0f, party);
    }
    REQUIRE(caught.size() == 1);
    REQUIRE(caught[0].pierces);
    REQUIRE(caught[0].subtype == Traps::kSpikes);
    REQUIRE(caught[0].position.x == 100.0f);
    REQUIRE(traps.update(2, 1.0f / 30.0f, party).empty());
}

TEST_CASE("barrels stand in the way until blows break them, each after its kind",
          "[game][world][fixtures]") {
    Fixture f("fixtures-barrels");
    Breakables barrels;
    REQUIRE(barrels.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(barrels.size() == 4); // one that holds a key, one that blows up, one of gas, one more
    barrels.setPlayerCount(1);
    REQUIRE(barrels.barrel(0).kind == BreakableStrike::Kind::Holding);
    REQUIRE(barrels.barrel(1).kind == BreakableStrike::Kind::Exploding);
    REQUIRE(barrels.barrel(2).kind == BreakableStrike::Kind::Poison);
    REQUIRE(barrels.barrel(3).kind == BreakableStrike::Kind::Plain);
    REQUIRE_FALSE(barrels.standing(3)); // for three players
    REQUIRE(barrels.obstacles().size() == 3);
    REQUIRE(barrels.within(Vec3{201.0f, 0.0f, 0.0f}, 3.0f) == std::vector<usize>{1, 2});
    REQUIRE(barrels.within(Vec3{201.0f, 40.0f, 0.0f}, 3.0f).empty());

    // A missile's path meets the nearer barrel first, and none when it flies over.
    REQUIRE(barrels.struckBy(Vec3{210.0f, 1.0f, 0.0f}, Vec3{190.0f, 1.0f, 0.0f}, 0.5f) ==
            std::optional<usize>{2});
    REQUIRE(barrels.struckBy(Vec3{190.0f, 1.0f, 0.0f}, Vec3{210.0f, 1.0f, 0.0f}, 0.5f) ==
            std::optional<usize>{1});
    REQUIRE_FALSE(
        barrels.struckBy(Vec3{210.0f, 9.0f, 0.0f}, Vec3{190.0f, 9.0f, 0.0f}, 0.5f).has_value());

    // Five hit points under an armour of one: a blow of three takes two, a feeble one one.
    std::optional<BreakableStrike> blow = barrels.strike(0, 3.0f);
    REQUIRE(blow.has_value());
    REQUIRE_FALSE(blow->broken);
    REQUIRE(barrels.barrel(0).health == 3);
    blow = barrels.strike(0, 0.5f);
    REQUIRE(barrels.barrel(0).health == 2);
    blow = barrels.strike(0, 30.0f);
    REQUIRE(blow->broken);
    REQUIRE(blow->kind == BreakableStrike::Kind::Holding);
    REQUIRE(blow->contents == 1); // the key it held
    REQUIRE(blow->position.x == 60.0f);
    REQUIRE_FALSE(barrels.standing(0));
    REQUIRE(barrels.obstacles().size() == 2);
    REQUIRE_FALSE(barrels.strike(0, 30.0f).has_value()); // broken, there is nothing to strike
    REQUIRE_FALSE(barrels.strike(3, 30.0f).has_value()); // nor one that is not there
    // Its staves stay lying; one that blew up leaves nothing.
    blow = barrels.strike(1, 30.0f);
    REQUIRE(blow->kind == BreakableStrike::Kind::Exploding);
    REQUIRE(blow->contents == -1);
    barrels.update(1.0f);
    REQUIRE(barrels.barrel(0).state == Breakables::kBroken);
    REQUIRE_FALSE(barrels.barrel(0).gone);
    REQUIRE(barrels.barrel(1).gone);
    REQUIRE(barrels.strike(2, 100)->broken);
    // A gas barrel, like one that blew up, is retired once its breaking has played.
    barrels.update(1);
    CHECK(barrels.barrel(2).gone);
    CHECK_FALSE(barrels.barrel(2).box.solid);
}

TEST_CASE("Stop Time retracts live traps and leaves a half-second rest on release",
          "[game][world][fixtures][stop-time]") {
    Fixture f("fixtures-time-stop");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7));
    traps.setPlayerCount(1);
    const std::array party{TrapVictim{Vec3{100, 0, 0}, 0.75f}};
    for (s32 frame = 0; frame < 120 && !traps.armed(0); ++frame) {
        traps.update(2, 1.0f / 30, party);
    }
    REQUIRE(traps.armed(0));
    for (s32 frame = 0; frame < 120; ++frame) {
        REQUIRE(traps.update(2, 1.0f / 30, party, true).empty());
        REQUIRE_FALSE(traps.armed(0));
    }
    CHECK(traps.trap(0).ticksLeft == 30);
    CHECK(traps.update(29, 29.0f / 60, party).empty());
    CHECK_FALSE(traps.armed(0));
    CHECK_FALSE(traps.update(1, 1.0f / 60, party).empty());
    CHECK(traps.armed(0));
}

TEST_CASE("potion magic stops a trap for 600 ticks, or disarms it for good",
          "[game][world][fixtures][magic-perks]") {
    Fixture f("fixtures-time-stop");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7));
    traps.setPlayerCount(1);
    const std::array party{TrapVictim{Vec3{100, 0, 0}, 0.75f}};
    for (s32 frame = 0; frame < 120 && !traps.armed(0); ++frame) {
        traps.update(2, 1.0f / 30, party);
    }
    REQUIRE(traps.armed(0));
    CHECK(traps.stop(0)); // out of its rest: it shows
    CHECK_FALSE(traps.armed(0));
    CHECK(traps.trap(0).ticksLeft == Traps::kStoppedRest);
    CHECK_FALSE(traps.stop(0)); // held again within sixty ticks: no show
    CHECK(traps.update(599, 599.0f / 60, party).empty());
    CHECK_FALSE(traps.armed(0));
    traps.update(1, 1.0f / 60, party);
    CHECK(traps.armed(0));

    // The greater perk: this archive has no SPIKES_D, so the trap goes altogether.
    REQUIRE(traps.disarm(0, f.device, f.layout, f.items, nullptr));
    CHECK(traps.trap(0).disarmed);
    CHECK(traps.trap(0).gone);
    CHECK_FALSE(traps.disarm(0, f.device, f.layout, f.items, nullptr));
    CHECK_FALSE(traps.stop(0));
    traps.setPlayerCount(1);
    CHECK_FALSE(traps.trap(0).shown);
    for (s32 frame = 0; frame < 600; ++frame) {
        REQUIRE(traps.update(2, 1.0f / 30, party).empty());
    }
}
TEST_CASE("chests pass the camera to their authored bomb sprites", "[fixtures][visual-parity]") {
    Fixture f("chest-camera");
    const auto directory = test::scratchDirectory("chest-camera-art");
    writeTextFile(directory / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"BOMB","file":"body.obj","meshTriangles":1}]})");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(directory / "animations.json", R"({"trees":[{"name":"CHEST",
      "nodes":[{"name":"FF_BOMB","object":"BOMB","parent":-1,"position":[0,0,0],"objectFlags":16783360}],
      "sequences":[{"name":"CLOSED","frames":0}]}]})");
    test::convertModelFixture(directory);
    REQUIRE(f.items.load(directory));
    Chests chests;
    REQUIRE(chests.bind(f.device, f.layout, f.items, nullptr));
    chests.setPlayerCount(1);
    for (const Vec3 eye : {Vec3{20, 12, -30}, Vec3{-20, 15, 30}}) {
        const auto camera = CameraFrame::at(eye);
        f.device.draws.clear();
        chests.draw(f.device, Mat4{1}, {}, &camera);
        REQUIRE_FALSE(f.device.draws.empty());
        const auto& vertices = f.device.draws.front().vertices;
        REQUIRE(vertices.size() == 3);
        const Vec3 normal = glm::normalize(glm::cross(vertices[1].position - vertices[0].position,
                                                      vertices[2].position - vertices[0].position));
        const Vec3 toward = glm::normalize(Vec3{eye.x, 0, eye.z});
        CHECK(std::abs(glm::dot(normal, toward)) == Approx(1).margin(0.0001));
    }
}

TEST_CASE("desert light walls play startup steady and reversed shutdown textures",
          "[fixtures][visual-parity][assets]") {
    const auto directory = test::assetOrSkip("ITEMS/LEVELC/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(directory));
    ItemFigure figure;
    REQUIRE(figure.place(device, items, "FORCEF", ItemInstance{}, nullptr));
    const auto texture = [&](std::string_view name) {
        const auto slot = items.textures.find(name);
        REQUIRE(slot.has_value());
        return &items.textures.texture(device, *slot);
    };
    const Texture* startup = texture("FFGEN00");
    const Texture* steady = texture("FFIELD00");
    const Texture* end = texture("FFGEN14");
    const auto shown = [&](const Texture* wanted) {
        device.draws.clear();
        figure.draw(device, Mat4{1}, {});
        return std::ranges::any_of(device.draws, [&](const auto& draw) {
            return draw.texture == wanted && draw.state.depthTest;
        });
    };
    figure.play(1, false);
    CHECK(shown(startup));
    figure.update(14.0f / 30);
    CHECK(shown(end));
    figure.play(2, false);
    CHECK(shown(steady));
    figure.play(3, false);
    CHECK(shown(end));
    figure.update(14.0f / 30);
    CHECK(shown(startup));
    figure.play(0, true);
    CHECK_FALSE(shown(startup));
    // OFF returns to the archive's one-frame global FFIELD/FFGEN defaults.
    CHECK(shown(steady));
    CHECK(shown(end));

    const auto dir = test::scratchDirectory("desert-forcefield-cycle");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":8,"subtype":2,"name":"FORCEF","activeOff":1}],
      "itemInstances":[{"info":0,"position":[0,0,0],"rotation":[0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    for (const s32 ticks : {1, 2}) {
        Traps traps;
        REQUIRE(traps.bind(device, layout, items, nullptr));
        traps.setPlayerCount(1);
        for (s32 cycle = 0; cycle < 2; ++cycle) {
            for (const s32 state : {0, 1, 2, 3}) {
                CAPTURE(ticks, cycle, state);
                REQUIRE(traps.trap(0).action == state);
                const s32 duration = std::array{2, 30, 40, 30}[static_cast<usize>(state)];
                REQUIRE(traps.trap(0).ticksLeft == duration);
                for (s32 elapsed = 0; elapsed < duration; elapsed += ticks) {
                    CHECK(traps.trap(0).action == state);
                    traps.update(ticks, static_cast<f32>(ticks) / 60, {});
                }
            }
        }
    }
}

TEST_CASE("C1 trapped chest bomb faces the camera throughout its fuse",
          "[fixtures][visual-parity][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    REQUIRE(layout.load(root / "LEVELS/LEVELC1"));
    REQUIRE(items.load(root / "ITEMS/LEVELC"));
    Chests chests;
    REQUIRE(chests.bind(device, layout, items, nullptr));
    chests.setPlayerCount(1);
    usize index = 0;
    while (index < chests.size() &&
           (!chests.chest(index).shown || chests.chest(index).subtype != Chests::kTrappedChest)) {
        ++index;
    }
    REQUIRE(index < chests.size());
    const auto& chest = chests.chest(index);
    const std::array party{ChestVisitor{chest.box.centre, 0.75f, 1}};
    chests.update(0, party);
    REQUIRE(chest.state == Chests::kOpening);
    const auto bomb = items.models.find("CHESTEXPFRM1");
    REQUIRE(bomb.has_value());
    const auto& mesh = items.models.mesh(*bomb);
    REQUIRE_FALSE(mesh.parts.empty());
    const Texture* skin = &items.textures.texture(device, mesh.parts.front().texture);
    for (s32 sample = 0; sample < 3; ++sample) {
        chests.update(0.3f, {});
        const auto transform = chest.figure.nodeTransform("FRM1");
        REQUIRE(transform.has_value());
        for (const Vec3 offset : {Vec3{20, 12, -30}, Vec3{-20, 15, 30}}) {
            const auto camera = CameraFrame::at(chest.figure.position() + offset);
            const Mat4 expected = camera.face(*transform, 1);
            device.draws.clear();
            chests.draw(device, Mat4{1}, {}, &camera);
            const auto found = std::ranges::find_if(device.draws, [&](const auto& draw) {
                return draw.texture == skin && !draw.vertices.empty() &&
                       glm::distance(
                           draw.vertices.front().position,
                           Vec3{expected *
                                Vec4{mesh.vertices[mesh.parts.front().indices[0]].position, 1}}) <
                           0.001f;
            });
            REQUIRE(found != device.draws.end());
            CHECK(found->state.depthTest);
        }
    }
}

} // namespace
