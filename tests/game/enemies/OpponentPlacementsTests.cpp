#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/OpponentPlacements.h"
#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void parameter(ItemInstance& instance, usize offset, s16 value) {
    const u16 bits = std::bit_cast<u16>(value);
    instance.params[offset] = static_cast<u8>(bits);
    instance.params[offset + 1] = static_cast<u8>(bits >> 8);
}
void sight(ItemInstance& instance, f32 value) {
    const auto bits = std::bit_cast<u32>(value);
    for (u32 shift = 0; shift < 32; shift += 8) {
        instance.params[4 + shift / 8] = static_cast<u8>(bits >> shift);
    }
}

TEST_CASE("ordinary placement resolution keeps native tuning fields and load order",
          "[rollback][opponent-placements]") {
    const s16 strength = GENERATE(0, 1, 3, 4);
    const s16 interval = GENERATE(-1, 0, 1, 7);
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = "GRU";
    info.radius = 2;
    info.height = 3;
    ItemInstance instance;
    instance.position = {2, 3, 5};
    instance.rotation = {0.3f, -0.4f, 0.2f};
    parameter(instance, 0, strength);
    parameter(instance, 2, -123);
    parameter(instance, 8, interval);
    sight(instance, 13.75f);
    const std::array roster{LevelEnemy{6, kMediumClass, {}}, LevelEnemy{9, kMediumOtherClass, {}}};
    std::vector<s32> loads;
    const auto result = OpponentPlacements::ordinary(instance, info, roster, 1, 1, [&](s32 kind) {
        loads.push_back(kind);
        return true;
    });
    REQUIRE(result);
    const s32 kind = strength >= 4 ? 9 : 6;
    CHECK(loads == std::vector{kind});
    CHECK(result->kind == kind);
    CHECK(result->spawn.kind == kind);
    CHECK(result->spawn.tier == (strength > 0 ? strength : 1));
    CHECK(result->spawn.algorithm == -123);
    CHECK(result->spawn.position == instance.position);
    CHECK(result->spawn.sight == 13.75f);
    CHECK(result->sight == 13.75f);
    CHECK(result->viewRadius == 6);
    CHECK(result->spawn.placed);
    CHECK(result->spawn.asleep == (strength == 0));
    CHECK(result->spawn.priority == EnemySpawn::Priority::Visible);
    CHECK_FALSE(result->carried);
    const Mat4 stood = itemPlacement(instance.position, instance.rotation);
    CHECK(result->spawn.direction == Vec3{stood[2][0], 0, stood[2][2]});
    CHECK(result->facing == portable::atan2(stood[2][0], stood[2][2]));
    const f32 expectedInterval = interval <= 0   ? EnemySpawn{}.throwInterval
                                 : interval == 1 ? 0
                                                 : 0.7f;
    CHECK(result->spawn.throwInterval == Catch::Approx(expectedInterval));
}

TEST_CASE("ordinary placement filters never invent a fallback resource or a great creature",
          "[rollback][opponent-placements]") {
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = "GRU";
    ItemInstance instance;
    s32 players = 1;
    bool allowsResource = true;
    usize expectedLoads = 0;
    SECTION("not a placed enemy") {
        info.type = ItemInfo::kPowerup;
    }
    SECTION("party minimum") {
        instance.minPlayers = 2;
    }
    SECTION("exact three-player exclusion") {
        instance.minPlayers = 13;
        players = 1;
    }
    SECTION("unknown native name") {
        info.name = "NOT_AN_ENEMY";
    }
    SECTION("golem belongs to the separate statue loader") {
        info.name = enemyKind(kGolemEnemyKind).name;
    }
    SECTION("gargoyle belongs to the separate statue loader") {
        info.name = enemyKind(kGargoyleEnemyKind).name;
    }
    SECTION("general belongs to the carried-item loader") {
        info.name = enemyKind(kGeneralEnemyKind).name;
    }
    SECTION("missing resource stays missing") {
        allowsResource = false;
        expectedLoads = 1;
    }
    usize loads = 0;
    CHECK_FALSE(OpponentPlacements::ordinary(instance, info, {}, players, players, [&](s32) {
        ++loads;
        return allowsResource;
    }));
    CHECK(loads == expectedLoads);
}

TEST_CASE("ordinary placement admits the swarm, Death and IT and no other kind",
          "[rollback][opponent-placements][enemy-traits]") {
    // A rat placement breeds the roster's small class, so the roster names each kind in turn;
    // a negative row is no class's and leaves the rat itself.
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = enemyKind(kRatKind).name;
    const ItemInstance instance;
    std::vector<s32> kinds{std::numeric_limits<s32>::min(), -1};
    for (s32 kind = 0; kind <= 45; ++kind) {
        kinds.push_back(kind);
    }
    kinds.push_back(std::numeric_limits<s32>::max());
    for (const s32 players : {1, 2}) {
        CAPTURE(players);
        std::vector<s32> placed;
        for (const s32 kind : kinds) {
            const std::array roster{LevelEnemy{kind, kSmallClass, {}}};
            std::vector<s32> loads;
            const auto placement = OpponentPlacements::ordinary(instance, info, roster, players,
                                                                players, [&](s32 loaded) {
                                                                    loads.push_back(loaded);
                                                                    return true;
                                                                });
            CAPTURE(kind);
            CHECK(loads.size() == (placement ? 1U : 0U));
            if (placement) {
                CHECK(loads.front() == placement->kind);
                placed.push_back(placement->kind);
            }
        }
        // Every swarm kind and Death; IT only with company; never the sentinel, a great one or a
        // code past the kinds.
        std::vector<s32> expected{3,  3,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                  14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 30};
        if (players > 1) {
            expected.push_back(31);
        }
        CHECK(placed == expected);
    }
}

TEST_CASE("IT placed for a party stands for the party a level is set up for only with company "
          "present",
          "[rollback][opponent-placements][it]") {
    // A rat placement breeds the roster's small class, here IT, placed for three.
    ItemInfo info;
    info.type = ItemInfo::kPlacedEnemy;
    info.name = enemyKind(kRatKind).name;
    ItemInstance instance;
    instance.minPlayers = 3;
    const std::array roster{LevelEnemy{kItKind, kSmallClass, {}}};
    const auto stands = [&](s32 players, s32 present) {
        return OpponentPlacements::ordinary(instance, info, roster, players, present,
                                            [](s32) { return true; })
            .has_value();
    };
    // The minimum goes by the party the level is set up for, as ItemVisible's test of the player
    // count would see it, the company by the players present (fn_80060114's gNumPlayers).
    CHECK_FALSE(stands(1, 3));
    CHECK_FALSE(stands(3, 1));
    CHECK(stands(3, 2));
    CHECK(stands(3, 3));
}

TEST_CASE("native placement loader retains missing-statue fallbacks and ordered carried claims",
          "[rollback][opponent-placements]") {
    const auto directory = test::scratchDirectory("opponent-placement-fallbacks");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[
        {"type":4,"name":"GOLEM","radius":2,"height":5},
        {"type":4,"name":"GENERAL","radius":1,"height":3},
        {"type":4,"name":"GAR","radius":4,"height":2},
        {"type":4,"name":"GRU","radius":1,"height":2}],
      "itemInstances":[
        {"info":0,"position":[1,2,3],"rotation":[0,0.2,0]},
        {"info":1,"position":[4,5,6],"rotation":[0,-0.4,0]},
        {"info":2,"position":[7,8,9]},
        {"info":1,"position":[4,5,6],"rotation":[0,-0.4,0]},
        {"info":2,"position":[9,9,9],"minPlayers":2},
        {"info":3,"position":[0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    // Unopened resource owners deliberately make the native statue/art lookups
    // fail. Great-one fallback rows survive; ordinary missing art does not.
    Enemies enemies;
    Critters critters;
    CritterStatues statues;
    std::vector<Vec3> claimed;
    const auto rows = OpponentPlacements::load(
        device, layout, {}, 1, 1, enemies, critters, statues, nullptr,
        [&](const Vec3& position, f32 reach, f32 rise) -> std::optional<usize> {
            CHECK(reach == 2);
            CHECK(rise == 3);
            claimed.push_back(position);
            return claimed.size() + 30;
        });
    REQUIRE(rows.size() == 4);
    CHECK(statues.count() == 0);
    CHECK(claimed == std::vector<Vec3>{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {4, 5, 6}});
    const std::array kinds{kGolemEnemyKind, kGeneralEnemyKind, kGargoyleEnemyKind,
                           kGeneralEnemyKind};
    for (usize index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto& instance = layout.itemInstances()[index];
        CHECK(row.kind == kinds[index]);
        CHECK(row.spawn.kind == EnemySpawn{}.kind);
        CHECK(row.spawn.position == instance.position);
        CHECK(row.spawn.placed == EnemySpawn{}.placed);
        CHECK(row.carried == std::optional<usize>{index + 31});
        const Mat4 stood = itemPlacement(instance.position, instance.rotation);
        CHECK(row.facing == portable::atan2(stood[2][0], stood[2][2]));
    }
    CHECK(rows[0].viewRadius == 10);
    CHECK(rows[1].viewRadius == 6);
    CHECK(rows[2].viewRadius == 8);
    CHECK(rows[1].spawn.position == rows[3].spawn.position);
    CHECK(rows[1].carried != rows[3].carried);
}

TEST_CASE("native statue placement loader preserves the authored floor-binding flag",
          "[rollback][opponent-placements][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    const auto directory = test::scratchDirectory("opponent-statue-floor-policy");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[
        {"type":4,"name":"GOLEM","radius":2,"height":5,"collisionFlags":0},
        {"type":4,"name":"GOLEM","radius":2,"height":5,"collisionFlags":1}],
      "itemInstances":[
        {"info":0,"position":[0,4,0]},
        {"info":1,"position":[20,4,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
         {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    test::FakeRenderDevice device;
    Enemies enemies;
    Critters critters;
    CritterStatues statues;
    critters.open(device, root, &collision, {}, 'G');
    const auto rows = OpponentPlacements::load(
        device, layout, {}, 1, 1, enemies, critters, statues, &collision,
        [](const Vec3&, f32, f32) -> std::optional<usize> { return std::nullopt; });
    CHECK(rows.empty());
    REQUIRE(statues.count() == 2);
    CHECK(statues.placement(0).onFloor);
    CHECK_FALSE(statues.placement(1).onFloor);
    CHECK(statues.positionOf(0).y == ItemFigure::kFloorLift);
    CHECK(statues.positionOf(1).y == 4);
    CritterStatues::LocalSnapshot saved;
    statues.saveLocal(saved);
    REQUIRE(statues.restoreLocal(saved));
    CritterStatues::LocalSnapshot restored;
    statues.saveLocal(restored);
    CHECK(statues.equalLocal(restored, saved));
}

TEST_CASE("Ancient Tree's placed archers and bombers keep their stump floor binding",
          "[opponent-placements][ancient-tree-placements][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELD3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("D3");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level());
    WorldCollision collision = world.collision();
    const auto restPose = itemSupportWorld(world.layout(), &collision);
    REQUIRE(restPose);
    Enemies enemies;
    enemies.open(device, root, &collision, 25, {}, 7);
    Critters critters;
    CritterStatues statues;
    const auto rows = OpponentPlacements::load(
        device, world.layout(), world.level()->enemies, 1, 1, enemies, critters, statues,
        &collision, [](const Vec3&, f32, f32) -> std::optional<usize> { return std::nullopt; });
    for (const usize instance : {101U, 102U, 106U, 109U, 137U}) {
        const Vec3 authored = world.layout().itemInstances().at(instance).position;
        const auto row = std::ranges::find_if(rows, [&](const auto& placement) {
            const Vec3& at = placement.spawn.position;
            return std::abs(at.x - authored.x) < 0.01f && std::abs(at.z - authored.z) < 0.01f;
        });
        CAPTURE(instance, authored.x, authored.y, authored.z);
        REQUIRE(row != rows.end());
        const auto* support = itemSupportAt(authored, &collision, &*restPose);
        REQUIRE(support);
        const auto floor = support->restingFloorAt(authored, 4, 10, 1);
        REQUIRE(floor);
        CAPTURE(floor->object, floor->y, row->support, row->spawn.position.y);
        CHECK(row->spawn.position.y == Catch::Approx(floor->y + ItemFigure::kFloorLift));
        if (!collision.moving(floor->object)) {
            CHECK(row->support == -1);
            continue;
        }
        CHECK(row->support == floor->object);

        // A pending placement must follow its branch before the camera admits it.
        // Losing this attachment leaves the enemy high over the lowered stump,
        // where the later birth's narrower six-unit floor probe can reject it.
        const auto stood = support->objectTransform(floor->object);
        REQUIRE(stood);
        const auto current = collision.objectTransform(floor->object);
        REQUIRE(current);
        const Mat4 moved = glm::translate(Mat4{1}, Vec3{0, -8, 0}) * *stood;
        collision.setObjectTransform(floor->object, moved);
        PlacedEnemies pending;
        pending.bind({*row}, &collision);
        const auto activations = pending.activations();
        REQUIRE(activations.size() == 1);
        const auto& activation = activations.front();
        const Vec3 expected{moved * row->supportLocal[3]};
        CHECK(glm::distance(activation.spawn.position, expected) < 0.001f);
        const auto born = enemies.spawn(activation.spawn, {});
        REQUIRE(born);
        CHECK(enemies.positionOf(*born).y == Catch::Approx(floor->y - 8).margin(0.01f));
        collision.setObjectTransform(floor->object, *current);
    }
}

} // namespace
