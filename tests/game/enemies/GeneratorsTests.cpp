#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/BossDefinition.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/screens/LevelOpponents.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;

/** A view from twenty back along -z, looking along +z at `target`, 60 degrees across. */
ViewVolume lookingAt(const Vec3& target) {
    ViewVolume view;
    view.position = target - Vec3{0.0f, 0.0f, 20.0f};
    return view;
}
constexpr f32 kStep = 1.0f / 30.0f;

void writeGeneratorArchive(const std::filesystem::path& root) {
    const auto archive = root / "MONSTERS/GRU";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 1\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1},
        {"index":1,"name":"GEN_GRU1L1","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"WALK","frames":10,"rate":30}]}]})");
}

TEST_CASE("generator visibility uses its expanded activation sphere and freezes distant timers",
          "[game][generators][generator-activation]") {
    const auto root = test::scratchDirectory("generator-activation-sphere");
    writeGeneratorArchive(root);
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[{"info":0,"minPlayers":1,"position":[0,0,0],
                        "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    ViewVolume view;
    view.position = {-35, 0, -20};
    // SetItem (0x800646f8) starts with 2*max(radius,height), then multiplies
    // generator visrad by four. Its body need not yet be inside the camera.
    REQUIRE_FALSE(view.sees({}, 10));
    REQUIRE(view.sees({}, 40));
    generators.setView(view);
    const std::array party{EnemyView{.position = {0, 0, 30}}};
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    const s32 countdown = generators.countdownOf(0);
    REQUIRE(countdown > 0);
    generators.setView(lookingAt({10000, 0, 0}));
    generators.update(countdown + kTicks, enemies, party);
    // do_items skips the normal generator dispatcher offscreen; generate_now
    // must not consume its wait until the generator enters the broad sphere again.
    CHECK(generators.countdownOf(0) == countdown);
    CHECK(generators.bredOf(0) == 1);
    generators.setView(view);
    generators.update(kTicks, enemies, party);
    CHECK(generators.countdownOf(0) == countdown - kTicks);
}

TEST_CASE("patrol generators create one sentry without ordinary brood visibility or quota gates",
          "[game][generators][generator-patrol]") {
    const auto root = test::scratchDirectory("generator-lone-sentry");
    writeGeneratorArchive(root);
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[{"info":0,"minPlayers":1,"position":[0,0,0],
                        "params":[1,0,15,0,5,0,20,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    GeneratorScales scales;
    scales.most = GENERATE(0.0f, 1.0f);
    REQUIRE(generators.bind(device, layout, enemies, nullptr, scales, 1));
    generators.setView(lookingAt({10000, 0, 0}));
    // generate_single (0x80063444), called before generate_now for AI15:
    // no nearby player required, one child, descriptor radius rather than height.
    generators.update(kTicks, enemies, {});
    REQUIRE(generators.bredOf(0) == 1);
    REQUIRE(enemies.alive(0));
    CHECK(enemies.generatorOf(0) == 0);
    CHECK(enemies.algorithmOf(0) == kPatrolWay);
    CHECK_FALSE(enemies.bred(0)); // birth_style 2 must not teach the ordinary brood lesson.
    // The native diagonal octants use 0.707, not an exact inverse square root.
    CHECK(glm::length(enemies.positionOf(0)) == Approx(2 + enemies.radiusOf(0)).margin(0.001f));
    CHECK(generators.countdownOf(0) == 0);
    const std::array party{EnemyView{.player = 0, .position = {0, 0, 30}}};
    generators.setView(lookingAt({}));
    for (s32 step = 0; step < 300; ++step) {
        generators.update(kTicks, enemies, party);
    }
    CHECK(generators.bredOf(0) == 1);
    CHECK(generators.livingOf(0) == 1);
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 10000;
    enemies.hurt(0, hit);
    generators.update(kTicks, enemies, {});
    CHECK(generators.algorithmOf(0) == 0);
    CHECK(generators.livingOf(0) == 0);
    generators.update(kTicks, enemies, party);
    CHECK(generators.bredOf(0) == (scales.most > 0 ? 2 : 1));
}

TEST_CASE("authored always-active generators retain their offscreen update exception",
          "[game][generators][generator-activation]") {
    const bool instanceFlag = GENERATE(false, true);
    const auto root = test::scratchDirectory("generator-always-active");
    writeGeneratorArchive(root);
    const std::string flags = instanceFlag ? R"("activeType":1)" : R"("activeType":65)";
    const std::string instance = instanceFlag ? R"("flags":1)" : R"("flags":0)";
    writeTextFile(root / "world.json",
                  R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10,)" +
                      flags + R"(}],"itemInstances":[{"info":0,"position":[0,0,0],)" + instance +
                      R"(,"params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    generators.setView(lookingAt({10000, 0, 0}));
    const std::array party{EnemyView{.position = {0, 0, 30}}};
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    const s32 countdown = generators.countdownOf(0);
    generators.update(kTicks, enemies, party);
    CHECK(generators.countdownOf(0) == countdown - kTicks);
}

TEST_CASE("generator durability uses authored armor and whole hit points",
          "[game][generators][generator-durability]") {
    const s32 armor = GENERATE(-1, 0, 3);
    const s32 player = GENERATE(-1, 0);
    const auto root = test::scratchDirectory("generator-authored-armor");
    writeGeneratorArchive(root);
    writeTextFile(root / "world.json",
                  R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":7,"armor":)" +
                      std::to_string(armor) + R"(}],
      "itemInstances":[{"info":0,"position":[0,0,0],
                        "params":[2,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    GeneratorScales scales;
    scales.health = 0.75f;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, scales, 1));
    // SetItem copies iteminfo.armor directly, then truncates scaled health.
    // fn_8005C1DC subtracts Round(power - armor), for environmental hits too.
    REQUIRE(generators.healthOf(0) == 10);
    if (armor < 0) {
        CHECK_FALSE(generators.strike(0, 10000, player));
        CHECK(generators.healthOf(0) == 10);
        CHECK(generators.stateOf(0) == 2);
        return;
    }
    REQUIRE(generators.strike(0, static_cast<f32>(armor) + 2.6f, player));
    CHECK(generators.healthOf(0) == 7);
    CHECK(generators.stateOf(0) == 2);
    REQUIRE(generators.strike(0, static_cast<f32>(armor) + 1.6f, player));
    CHECK(generators.healthOf(0) == 5);
    CHECK(generators.stateOf(0) == 1);
    REQUIRE(generators.strike(0, static_cast<f32>(armor), player));
    CHECK(generators.healthOf(0) == 4); // Nonpositive remainder becomes one before Round.
    REQUIRE(generators.strike(0, static_cast<f32>(armor) + 0.49f, player));
    CHECK(generators.healthOf(0) == 4);
    REQUIRE(generators.strike(0, static_cast<f32>(armor) + 0.5f, player));
    CHECK(generators.healthOf(0) == 3);
    REQUIRE(generators.strike(0, 10000, player));
    CHECK(generators.healthOf(0) == 0);
    CHECK_FALSE(generators.standing(0));
}

TEST_CASE("wall generators share their authored facing across rendering collision and spawning",
          "[game][generators][item-orientation]") {
    const auto root = test::scratchDirectory("wall-generator-orientation");
    writeGeneratorArchive(root);
    writeTextFile(root / "world.json", R"({
        "objects":[{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
        "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                      "xSize":3,"zSize":1,"hitPoints":10}],
        "itemInstances":[
            {"info":0,"minPlayers":1,"position":[10,3,20],"rotation":[0,1.57079637,0],
             "params":[1,0,7,0,5,0,20,0,0,0,0,0]},
            {"info":0,"minPlayers":1,"position":[180,3,20],"rotation":[0.3,-0.7,0.2],
             "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root));
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    REQUIRE(generators.count() == 2);
    generators.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    const std::array vertices{Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    for (usize i = 0; i < generators.count(); ++i) {
        const auto& instance = layout.itemInstances()[i];
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        REQUIRE(device.draws[i].vertices.size() == vertices.size());
        for (usize v = 0; v < vertices.size(); ++v) {
            CHECK(glm::distance(device.draws[i].vertices[v].position,
                                Vec3{placement * Vec4{vertices[v], 1}}) < 0.0001f);
        }
    }
    // A positive authored quarter-turn points out of the wall toward -X, not +X.
    CHECK(device.draws[0].vertices[0].position.x == Approx(9));
    CHECK(generators.boxOf(0).yaw == Approx(-1.57079637f));
    const std::array party{EnemyView{.position = Vec3{0, 3, 20}}};
    // Only the one on screen breeds.
    generators.setView(lookingAt(generators.positionOf(0)));
    generators.update(2, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    REQUIRE(generators.bredOf(1) == 0);
    bool found = false;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id) && enemies.generatorOf(id) == 0) {
            found = true;
            CHECK(enemies.positionOf(id).x < generators.positionOf(0).x);
        }
    }
    REQUIRE(found);
}

TEST_CASE("generators retain authored support offsets through platform motion and collision holds",
          "[game][generators][generator-platform]") {
    const auto root = test::scratchDirectory("generator-platform");
    writeGeneratorArchive(root);
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"LIFT","position":[10,5,20],"flags":4102}],
      "itemInfos":[
        {"type":3,"name":"GRU","radius":2,"height":5,"xSize":3,"zSize":1,"hitPoints":10},
        {"type":3,"name":"GRU","radius":2,"height":5,"collisionFlags":1,"hitPoints":10}],
      "itemInstances":[
        {"info":0,"minPlayers":1,"position":[12,5,23],"rotation":[0,0,0],
         "params":[1,0,7,0,5,0,20,0,0,0,0,0]},
        {"info":1,"minPlayers":1,"position":[15,5,23],"rotation":[0,0,0],
         "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    WorldCollision collision;
    collision.build(
        {CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{30, 0, 30}, Vec3{30, 0, -30}},
                           .object = 0},
         CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{-30, 0, 30}, Vec3{30, 0, 30}},
                           .object = 0}});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{10, -15, 20}));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, &collision, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
    REQUIRE(generators.count() == 2);
    CHECK(glm::distance(generators.positionOf(0), Vec3{12, -15 + ItemFigure::kFloorLift, 23}) <
          0.001f);
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{30, 10, 40}), 1.57079637f, Vec3{0, 1, 0});
    collision.setSolid(0, false);
    collision.setObjectTransform(0, moved);
    generators.syncFloors();
    const Vec3 expected{moved * Vec4{2, ItemFigure::kFloorLift, 3, 1}};
    CHECK(glm::distance(generators.positionOf(0), expected) < 0.001f);
    CHECK(glm::distance(generators.boxOf(0).centre, expected) < 0.001f);
    CHECK(generators.boxOf(0).yaw == Approx(1.57079637f));
    // The collision-flag exception overlaps the same authored floor, but stays fixed.
    CHECK(generators.positionOf(1) == Vec3{15, 5, 23});
    generators.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    const Vec3 front{moved * Vec4{2, ItemFigure::kFloorLift, 4, 1}};
    CHECK(glm::distance(device.draws[0].vertices[0].position, front) < 0.001f);
    // Births use the carried center and facing once the floor becomes walkable again.
    collision.setSolid(0, true);
    generators.setView(lookingAt(expected));
    const std::array party{EnemyView{.position = expected + Vec3{15, 0, 0}}};
    generators.update(2, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    REQUIRE(enemies.alive(0));
    CHECK(enemies.positionOf(0).x > expected.x);
    CHECK(enemies.positionOf(0).y == Approx(10).margin(0.2f));
    const auto hit = generators.strike(0, 1000, 0);
    REQUIRE(hit.has_value());
    CHECK(glm::distance(hit->position, expected) < 0.001f);
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{40, 20, 50}));
    generators.syncFloors();
    CHECK(glm::distance(generators.positionOf(0), Vec3{42, 20 + ItemFigure::kFloorLift, 53}) <
          0.001f);
    CHECK_FALSE(generators.boxOf(0).solid);
}

TEST_CASE("the Sky Dominion generator rides its authored animated floor",
          "[game][generators][generator-platform][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELK1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("K1");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Enemies enemies;
    enemies.open(device, root, &world.collision(), Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, world.layout(), enemies, &world.collision(), {}, 1,
                            world.level()->enemies, level->realmId, &world.items()));
    const Vec3 authored = world.layout().itemInstances().at(317).position;
    s32 id = -1;
    for (s32 i = 0; i < static_cast<s32>(generators.count()); ++i) {
        const Vec3 at = generators.positionOf(i);
        if (glm::length(Vec2{at.x - authored.x, at.z - authored.z}) < 0.1f) {
            id = i;
            break;
        }
    }
    REQUIRE(id >= 0);
    const auto support = world.collision().objectTransform(164);
    REQUIRE(support.has_value());
    const Vec3 start = generators.positionOf(id);
    const Vec4 local = glm::inverse(*support) * Vec4{start, 1};
    f32 traveled = 0;
    for (s32 tick = 0; tick < 180; ++tick) {
        world.update(kStep);
        generators.syncFloors();
        const auto current = world.collision().objectTransform(164);
        REQUIRE(current.has_value());
        const Vec3 expected{*current * local};
        CHECK(glm::distance(generators.positionOf(id), expected) < 0.001f);
        traveled = std::max(traveled, glm::distance(start, expected));
    }
    CHECK(traveled > 0.1f);
}

TEST_CASE("generators alternate successful zig-zag births without turning their bodies",
          "[game][generators][courtyard-grunt][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto fixture = test::scratchDirectory("zig-zag-generator");
    writeTextFile(fixture / "world.json", R"({
        "objects":[{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
        "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                      "xSize":3,"zSize":1,"hitPoints":10}],
        "itemInstances":[{"info":0,"minPlayers":1,"position":[0,0,0],
            "params":[1,0,14,0,1,0,1,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(fixture));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    REQUIRE(generators.count() == 1);
    const std::array party{EnemyView{.player = 0, .position = Vec3{0, 0, 20}}};
    for (const s32 side : {1, -1, 1}) {
        for (s32 tick = 0; tick < 120 && !enemies.alive(0); ++tick) {
            generators.update(1, enemies, party);
        }
        REQUIRE(enemies.alive(0));
        const auto& memory = enemies.memoryOf(0);
        CHECK(memory.zigZag.side == side);
        CHECK(wrapAngle(memory.heading - enemies.yawOf(0)) ==
              Approx(static_cast<f32>(side) * 0.7853981635f));
        CHECK(memory.headingBefore == memory.heading);
        // A full brood must not advance the alternation on its failed/not-attempted births.
        const s32 born = generators.bredOf(0);
        for (s32 tick = 0; tick < 60; ++tick) {
            generators.update(1, enemies, party);
        }
        CHECK(generators.bredOf(0) == born);
        EnemyHit hit;
        hit.damage = 100000;
        enemies.hurt(0, hit);
        for (s32 frame = 0; frame < 180; ++frame) {
            enemies.update(2, kStep, party);
        }
        REQUIRE_FALSE(enemies.alive(0));
    }
    CHECK(generators.bredOf(0) == 3);
}

TEST_CASE("Dream generators retain the floor clearance of their portal artwork",
          "[game][generators][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ4/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("J4");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    Enemies enemies;
    enemies.open(device, root, &world.collision(), Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, world.layout(), enemies, &world.collision(), {}, 1,
                            world.level()->enemies, 10, &world.items()));
    usize grounded = 0;
    for (s32 i = 0; i < static_cast<s32>(generators.count()); ++i) {
        const Vec3 position = generators.positionOf(i);
        if (const auto floor = world.collision().floorAt(position, 0.5f, 1.0f)) {
            CHECK(position.y - floor->y == Approx(0.1f).margin(0.0001f));
            ++grounded;
        }
    }
    REQUIRE(grounded > 0);
}

TEST_CASE("Temple and Underworld special generators draw their authored trees and breed",
          "[game][generators][assets]") {
    const s32 realm = GENERATE(5, 6);
    const bool temple = realm == 5;
    const auto* level = temple ? "LEVELS/LEVELE1" : "LEVELS/LEVELF1";
    const auto* archive = temple ? "ITEMS/LEVELE" : "ITEMS/LEVELF";
    const auto root = test::assetOrSkip(std::string(level) + "/WORLDS.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::assetOrSkip(std::string(archive) + "/ANIM.PS2");
    const std::array templeKinds{"ICE", "IMP", "PLA", "ZOM"};
    const std::array hellKinds{"DEM", "WAR", "GHO", "SKY"};
    for (const auto* kind : temple ? templeKinds : hellKinds) {
        test::assetOrSkip(std::string("MONSTERS/") + kind + "/ANIM.PS2");
    }
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / level));
    ItemArchive items;
    REQUIRE(items.load(root / archive));
    Enemies enemies;
    enemies.open(device, root, nullptr, Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, {}, realm, &items));
    usize expected = 0;
    usize shown = 0;
    for (const auto& instance : layout.itemInstances()) {
        if (layout.itemInfos()[static_cast<usize>(instance.info)].type == ItemInfo::kGenerator) {
            ++expected;
            shown += shownToParty(instance.minPlayers, 1) ? 1 : 0;
        }
    }
    REQUIRE(expected > 0);
    REQUIRE(generators.count() == expected);
    CHECK(generators.obstacles().size() == shown);
    for (s32 i = 0; i < static_cast<s32>(generators.count()); ++i) {
        CHECK(generators.kindOf(i) == (temple ? -2 : -3));
        CHECK(generators.tierOf(i) == (temple ? 2 : 3));
        CHECK(generators.stateOf(i) == (temple ? 2 : 3));
        CHECK(generators.bodyShown(i) == generators.standing(i));
    }
    s32 chosen = 0;
    while (static_cast<usize>(chosen) < generators.count() && !generators.standing(chosen)) {
        ++chosen;
    }
    REQUIRE(static_cast<usize>(chosen) < generators.count());
    const std::array views{EnemyView{.position = generators.positionOf(chosen)}};
    generators.update(2, enemies, views);
    REQUIRE(enemies.count() > 0);
    generators.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    const s32 initialState = temple ? 2 : 3;
    const f32 stateHealth = generators.healthOf(chosen) / static_cast<f32>(initialState);
    for (s32 state = initialState - 1; state >= 0; --state) {
        REQUIRE(generators.strike(chosen, stateHealth, 0).has_value());
        CHECK(generators.stateOf(chosen) == state);
        CHECK(generators.bodyShown(chosen));
        device.draws.clear();
        generators.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.texture != &device.whiteTexture());
        }
    }
    CHECK(generators.stateOf(chosen) == 0);
    CHECK(generators.bodyShown(chosen)); // broken ruin remains
}

TEST_CASE("special generators preserve authored durability while their troops keep realm strength",
          "[game][generators][generator-special-birth][assets]") {
    const s32 realm = GENERATE(5, 6);
    const bool temple = realm == 5;
    const auto* archive = temple ? "ITEMS/LEVELE" : "ITEMS/LEVELF";
    const auto root = test::assetOrSkip(std::string(archive) + "/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    const std::array templeKinds{"ICE", "IMP", "PLA", "ZOM"};
    const std::array hellKinds{"DEM", "WAR", "GHO", "SKY"};
    for (const auto* kind : temple ? templeKinds : hellKinds) {
        test::assetOrSkip(std::string("MONSTERS/") + kind + "/ANIM.PS2");
    }
    const auto fixture = test::scratchDirectory("generator-special-birth");
    writeTextFile(fixture / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"SPECIAL","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[{"info":0,"position":[0,0,0],
                        "params":[2,0,7,0,0,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(fixture));
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(root / archive));
    Enemies enemies;
    enemies.open(device, root, nullptr, 8, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, {}, realm, &items));
    // SetItem applies the authored tier's durability/defaults before replacing the
    // special generator's state. F1's authored tier is two even though troops are three.
    REQUIRE(generators.healthOf(0) == 20);
    REQUIRE(generators.mostOf(0) == 5);
    REQUIRE(generators.intervalOf(0) == 10);
    REQUIRE(generators.tierOf(0) == (temple ? 2 : 3));
    REQUIRE(generators.strike(0, 11, 0));
    REQUIRE(generators.stateOf(0) == 1);
    REQUIRE(generators.healthOf(0) == 9);
    const std::array party{EnemyView{.position = {0, 0, -30}}};
    // All forward humanoid directions are blocked. generate_enemy switches on the
    // unresolved -2/-3 type, allowing the other bearings even for PLA/WAR/SKY.
    const std::array blockers{
        Obstacle{.centre = {0, 0, 3}, .halfAcross = 30, .halfAlong = 1, .height = 50}};
    const std::array templeSpecies{16, 23, 14, 13};
    const std::array hellSpecies{2, 24, 20, 25};
    for (s32 birth = 0; birth < 3; ++birth) {
        if (generators.countdownOf(0) > 0) {
            generators.update(generators.countdownOf(0), enemies, party, blockers);
        }
        generators.update(kTicks, enemies, party, blockers);
        REQUIRE(generators.bredOf(0) == birth + 1);
        REQUIRE(enemies.alive(birth));
        CHECK(enemies.kindOf(birth) ==
              (temple ? templeSpecies : hellSpecies)[static_cast<usize>(birth)]);
        CHECK(enemies.tierOf(birth) == (temple ? 2 : 3));
        CHECK(enemies.positionOf(birth).z <= 0.01f);
    }
}

/** A field with one grunt generator of strength two at the origin facing +z, one of strength
 * three at x 60 for a party of two, and a rats' one at x 120. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 3, "name": "GRU", "radius": 2, "height": 5, "collisionType": 1, "hitPoints": 10,
     "activeType": 5, "activeOff": -30, "activeOn": 1, "armor": 3},
    {"type": 3, "name": "RAT", "radius": 2, "height": 5, "collisionType": 1, "hitPoints": 10},
    {"type": 3, "name": "BOSSGEN", "radius": 2, "height": 5, "hitPoints": 10}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [2, 0, 7, 0, 5, 0, 20, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 2, "position": [60, 0, 0], "rotation": [0, 0, 0],
     "params": [3, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 1, "position": [120, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 2, "minPlayers": 1, "position": [180, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}
  ]
})");
    return dir;
}

TEST_CASE("patrol offspring alert their generator and release quota before their corpse vanishes",
          "[generators][enemies][generator-feedback][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto fixture = test::scratchDirectory("generator-patrol-feedback");
    writeTextFile(fixture / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
      "animations":[],"particles":[],"locators":[],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[{"info":0,"minPlayers":1,"position":[0,0,0],"rotation":[0,0,0],
                        "params":[1,0,15,0,2,0,2,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(fixture));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 13, {}, 3);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.generator = 0;
    spawn.algorithm = 15;
    spawn.placed = true;
    spawn.position = {20, 0, 0};
    const auto patrol = enemies.spawn(spawn, {});
    REQUIRE(patrol.has_value());
    spawn.algorithm = 7;
    spawn.position.x = 40;
    REQUIRE(enemies.spawn(spawn, {}).has_value());
    generators.update(2, enemies, {});
    REQUIRE(generators.livingOf(0) == 2);
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 1;
    enemies.hurt(*patrol, hit);
    generators.update(2, enemies, {});
    CHECK(generators.algorithmOf(0) == 0);
    CHECK(generators.livingOf(0) == 2);
    CHECK(enemies.algorithmOf(*patrol) == 15); // siblings are not rewritten
    hit.damage = 10000;
    enemies.hurt(*patrol, hit);
    generators.update(2, enemies, {});
    CHECK(generators.livingOf(0) == 0); // patrol uncoupling resets, not merely decrements
    CHECK(enemies.generatorOf(*patrol) == -1);
    CHECK(enemies.bred(*patrol));
    const std::array party{EnemyView{.player = 3, .position = {0, 0, 100}}};
    generators.update(2, enemies, party);
    CHECK(generators.bredOf(0) == 1);
    CHECK(generators.livingOf(0) == 1);
    // A lingering corpse must not release that new birth's quota a second time.
    for (s32 frame = 0; frame < 180; ++frame) {
        enemies.update(2, kStep, {});
    }
    generators.update(2, enemies, {});
    CHECK(generators.livingOf(0) == 1);
}

TEST_CASE("generator population changes wait for offscreen without losing brood or damage",
          "[generators][multiplayer][generator-presence][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(sampleLevel("generator-population-change")));
    Enemies enemies;
    enemies.open(device, root, nullptr, 13, {}, 3);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 2));
    const s32 id = 1;
    const Vec3 at = generators.positionOf(id);
    const std::array party{EnemyView{.position = at + Vec3{0, 0, 30}}};
    generators.setView(lookingAt(at));
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(id) == 1);
    REQUIRE(enemies.generatorOf(0) == id);
    REQUIRE(generators.strike(id, 2, 0));
    const f32 health = generators.healthOf(id);
    generators.setPlayerCount(1);
    generators.update(kTicks, enemies, party);
    CHECK(generators.standing(id));
    CHECK(generators.bodyShown(id));
    const s32 wait = generators.countdownOf(id);
    generators.setView(lookingAt({10000, 0, 0}));
    generators.update(kTicks, enemies, party);
    CHECK_FALSE(generators.standing(id));
    CHECK_FALSE(generators.bodyShown(id));
    CHECK_FALSE(generators.boxOf(id).solid);
    CHECK(generators.healthOf(id) == health);
    CHECK(generators.countdownOf(id) == wait);
    CHECK(enemies.alive(0));
    CHECK(enemies.generatorOf(0) == id);
    CHECK_FALSE(generators.strike(id, 1000, 0));
    CHECK(generators.within(at, 5).empty());
    CHECK_FALSE(generators.struckBy(at + Vec3{-8, 2, 0}, at + Vec3{8, 2, 0}, 0.5f));
    CHECK(generators.obstacles().size() == 2);
    // Returning to the camera does not resurrect a now-ineligible generator.
    generators.setView(lookingAt(at));
    generators.update(kTicks, enemies, party);
    CHECK_FALSE(generators.standing(id));
    CHECK(enemies.alive(0));
}

TEST_CASE("exact-party generators can appear offscreen but never pop into an observed area",
          "[generators][multiplayer][generator-presence][assets]") {
    const bool encountered = GENERATE(false, true);
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto dir = test::scratchDirectory("generator-exact-party");
    writeTextFile(dir / "world.json", R"({
        "objects": [{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
        "animations": [], "particles": [], "locators": [],
        "itemInfos": [{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
        "itemInstances": [{"info":0,"minPlayers":12,"position":[0,0,0],
                           "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]
    })");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    Enemies enemies;
    enemies.open(device, root, nullptr, 13, {}, 3);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 3));
    REQUIRE(generators.count() == 1);
    REQUIRE_FALSE(generators.standing(0));
    generators.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    if (encountered) {
        generators.setView(lookingAt({}));
        generators.update(kTicks, enemies, {});
    }
    generators.setView(lookingAt({10000, 0, 0}));
    generators.setPlayerCount(2);
    generators.update(kTicks, enemies, {});
    CHECK(generators.standing(0) == !encountered);
    CHECK(generators.bodyShown(0) == !encountered);
    CHECK(generators.boxOf(0).solid == !encountered);
    CHECK(generators.obstacles().size() == (encountered ? 0 : 1));
    CHECK(generators.enemyObstacles().size() == (encountered ? 0 : 1));
    generators.setView(lookingAt({}));
    const std::array party{EnemyView{.position = Vec3{0, 0, 30}}};
    generators.update(kTicks, enemies, party);
    CHECK(generators.bredOf(0) == (encountered ? 0 : 1));
    device.draws.clear();
    generators.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty() == encountered);
}

TEST_CASE("boss generators use the stage record and breed after the birth delay",
          "[spider][assets][stop-time]") {
    const auto root =
        test::assetOrSkip("MONSTERS/SPI/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 17, {}, 3);
    Generators generators;
    ItemArchive noArt;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 10;
    info.height = 5;
    info.radius = 2;
    info.armor = 0;
    REQUIRE(generators.placeBoss(device, info, noArt, enemies, 9, Mat4{1}, nullptr));
    REQUIRE(generators.count() == 1);
    REQUIRE(generators.kindOf(0) == 9);
    REQUIRE(generators.healthOf(0) == 10);
    REQUIRE(generators.tierOf(0) == 1);
    REQUIRE(generators.mostOf(0) == 10);
    REQUIRE(generators.intervalOf(0) == 5);
    REQUIRE(generators.countdownOf(0) == 40);
    REQUIRE_FALSE(generators.bodyShown(0));
    const std::vector<EnemyView> party{{0, {0, 0, 20}, 1, 6}};
    generators.update(20, enemies, party, {}, true);
    CHECK(generators.countdownOf(0) == 20);
    generators.update(20, enemies, party, {}, true);
    CHECK(generators.countdownOf(0) == 0);
    generators.update(600, enemies, party, {}, true);
    REQUIRE(generators.bredOf(0) == 0);
    generators.update(2, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    const auto event = generators.strike(0, 10, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->destroyed);
    generators.update(600, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
}

TEST_CASE("boss generator artwork distinguishes a landed egg from looping bodies",
          "[game][generators]") {
    CHECK(bossGeneratorVisual(37).tree == "GENPROJHIT");
    CHECK(bossGeneratorVisual(37).settled);
    CHECK(bossGeneratorVisual(41).tree == "ATK12GEN");
    CHECK_FALSE(bossGeneratorVisual(41).settled);
    CHECK(bossGeneratorVisual(36).tree == "BOSSGEN");
    CHECK_FALSE(bossGeneratorVisual(36).settled);
}

TEST_CASE("boss generators can borrow their body from the summoned species archive",
          "[game][generators][genie-generators]") {
    const auto root = test::scratchDirectory("boss-generator-brood-art");
    const auto archive = root / "MONSTERS/WIND";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BOSSGEN",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"ACTIVE","frames":30,"frameRate":30,"repeats":true}]}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    ItemArchive supplied;
    SECTION("missing level and boss art falls back to the summoned species") {}
    SECTION("explicitly supplied art keeps precedence") {
        REQUIRE(supplied.load(archive));
    }
    Generators generators;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 10;
    info.radius = 2;
    info.height = 5;
    constexpr s32 kWindKind = 26;
    const Vec3 position{10, 3, 20};
    REQUIRE(generators.placeBoss(device, info, supplied, enemies, kWindKind,
                                 glm::translate(Mat4{1}, position), nullptr));
    REQUIRE(generators.bodyShown(0));
    generators.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 3);
    CHECK(device.draws.front().vertices.front().position == position);
    ItemArchive* expected = supplied.loaded() ? &supplied : enemies.archive(kWindKind);
    REQUIRE(expected != nullptr);
    CHECK(device.draws.front().texture == &expected->textures.texture(device, 0));
    const auto hit = generators.strike(0, 1000, 0);
    REQUIRE(hit);
    REQUIRE(hit->destroyed);
    device.draws.clear();
    generators.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
}

TEST_CASE("Genie whirlwind attacks leave visible animated generators that breed wind enemies",
          "[game][generators][genie-generators][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/DJINN/ANIM.PS2");
    test::assetOrSkip("MONSTERS/WIND/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("C5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {47.203125f, 10, -1.203125f}, -1.5707963f);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.bosses().wake();
    // WINDGEN becomes available at rate .75, below roughly 94% health.
    EnemyHit phase;
    phase.damage = opponents.bosses().view().maxHealth * 0.1f;
    opponents.bosses().hurt(phase);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    REQUIRE(opponents.generators().count() == 0);
    bool bred = false;
    for (s32 frame = 0; frame < 9000 && !bred; ++frame) {
        opponents.update(2, kStep, players, {}, events);
        effects.update(kStep);
        for (usize i = 0; i < opponents.generators().count(); ++i) {
            bred |= opponents.generators().bredOf(static_cast<s32>(i)) > 0;
        }
    }
    REQUIRE(opponents.generators().count() > 0);
    REQUIRE(bred);
    auto& generators = opponents.generators();
    ItemArchive* wind = opponents.enemies().archive(26);
    REQUIRE(wind != nullptr);
    REQUIRE(wind->trees.find("BOSSGEN"));
    for (usize i = 0; i < generators.count(); ++i) {
        CHECK(generators.kindOf(static_cast<s32>(i)) == 26);
        REQUIRE(generators.bodyShown(static_cast<s32>(i)));
    }
    const auto vertices = [&] {
        device.draws.clear();
        generators.draw(device, Mat4{1}, {});
        std::vector<Vec3> result;
        for (const auto& draw : device.draws) {
            CHECK(draw.texture != nullptr);
            for (const auto& vertex : draw.vertices) {
                result.push_back(vertex.position);
            }
        }
        return result;
    };
    const auto first = vertices();
    REQUIRE_FALSE(first.empty());
    generators.update(10, opponents.enemies(), {}, {}, true);
    const auto next = vertices();
    CHECK(next.size() == first.size());
    CHECK(next != first);
    for (usize i = 0; i < generators.count(); ++i) {
        const auto hit = generators.strike(static_cast<s32>(i), 100000, 0);
        REQUIRE(hit);
        CHECK(hit->destroyed);
    }
    CHECK(vertices().empty());
}

TEST_CASE("Spider Queen generators retain the landed egg pose until destroyed",
          "[spider][spider-generator][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/DRIDER/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/SPI/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root / "MONSTERS/DRIDER"));
    Enemies enemies;
    enemies.open(device, root, nullptr, 17, {}, 3);
    Generators generators;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 10;
    info.height = 5;
    info.radius = 2;
    const auto visual = bossGeneratorVisual(37);
    CHECK(visual.tree == "GENPROJHIT");
    REQUIRE(visual.settled);
    REQUIRE(generators.placeBoss(device, info, archive, enemies, 9, Mat4{1}, nullptr, visual.tree,
                                 visual.settled));
    REQUIRE(generators.bodyShown(0));
    const auto vertices = [&] {
        device.draws.clear();
        generators.draw(device, Mat4{1}, WorldLighting{});
        std::vector<Vec3> result;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                result.push_back(vertex.position);
            }
        }
        return result;
    };
    const auto landed = vertices();
    REQUIRE_FALSE(landed.empty());
    // Compare against the actual last pose of the same impact that left this egg.
    ItemFigure impact;
    const ItemInstance instance;
    REQUIRE(impact.place(device, archive, "GENPROJHIT", instance, nullptr));
    impact.play(0, false);
    impact.update(static_cast<f32>(impact.ticksOf(0)) / 60);
    REQUIRE(impact.finished());
    const auto egg = impact.nodeTransform("L1");
    REQUIRE(egg);
    CHECK(glm::length(Vec3{(*egg)[0]}) > 0.99f);
    CHECK(glm::length(Vec3{(*egg)[1]}) > 0.99f);
    device.draws.clear();
    impact.draw(device, Mat4{1}, WorldLighting{});
    std::vector<Vec3> expected;
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            expected.push_back(vertex.position);
        }
    }
    CHECK(landed == expected);
    // No repeated shrink/grow startup while it breeds or Stop Time is active.
    for (s32 frame = 0; frame < 90; ++frame) {
        generators.update(2, enemies, {}, {}, frame < 45);
        REQUIRE(vertices() == landed);
    }
    const auto event = generators.strike(0, 1000, 0);
    REQUIRE(event);
    REQUIRE(event->destroyed);
    CHECK_FALSE(generators.bodyShown(0));
    CHECK(vertices().empty());
}

TEST_CASE("the fields place forty-seven generators for a party of one, of grunts and rats",
          "[game][enemies][assets]") {
    const std::filesystem::path level =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path();
    const std::filesystem::path root = level.parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(level));
    WorldCollision collision;
    REQUIRE(collision.load(level, layout));
    Enemies enemies;
    enemies.open(device, root, &collision, 13, EnemyScales{}, 3);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, &collision, GeneratorScales{}, 1));
    REQUIRE(generators.count() == 117);
    REQUIRE(generators.obstacles().size() == 47);
    REQUIRE(enemies.kindLoaded(kGruntKind));
    REQUIRE(enemies.kindLoaded(kRatKind));
    // With the fields' roster the same records breed zombies and maggots instead.
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    const std::vector<LevelEnemy> fields{{13, kMediumClass, {}}, {12, kSmallClass, {}}};
    Enemies bred;
    bred.open(device, root, &collision, 13, EnemyScales{}, 3);
    Generators graves;
    REQUIRE(graves.bind(device, layout, bred, &collision, GeneratorScales{}, 1, fields));
    REQUIRE(graves.count() == 117);
    REQUIRE(graves.obstacles().size() == 47);
    REQUIRE(bred.kindLoaded(13));
    REQUIRE(bred.kindLoaded(12));
    REQUIRE_FALSE(bred.kindLoaded(kGruntKind));
    REQUIRE(graves.kindOf(0) == 13);
    REQUIRE(graves.bodyShown(0));
    Generators wider;
    REQUIRE(wider.bind(device, layout, enemies, &collision, GeneratorScales{}, 4));
    REQUIRE(wider.count() == 117);
    REQUIRE(wider.obstacles().size() == 117);
    // All records retain their state, including those hidden by the party-size gate.
    for (usize g = 0; g < generators.count(); ++g) {
        const auto id = static_cast<s32>(g);
        REQUIRE(generators.stateOf(id) == generators.tierOf(id));
        REQUIRE(generators.tierOf(id) >= 1);
        REQUIRE(generators.tierOf(id) <= 3);
        REQUIRE(generators.mostOf(id) > 0);
        REQUIRE(generators.intervalOf(id) > 0);
    }
    REQUIRE(generators.obstacles().size() == 47);
    const Vec3 at = generators.positionOf(0);
    REQUIRE((generators.struckBy(at + Vec3{-8.0f, 2.0f, 0.0f}, at + Vec3{8.0f, 2.0f, 0.0f}, 0.5f) ==
             0));
    REQUIRE_FALSE(generators.within(at + Vec3{0.0f, 2.0f, 0.0f}, 1.0f).empty());
    REQUIRE_FALSE(
        generators.struckBy(at + Vec3{-8.0f, 40.0f, 0.0f}, at + Vec3{8.0f, 40.0f, 0.0f}, 0.5f)
            .has_value());
}

TEST_CASE("a generator breeds grunts for a party near it up to its count, and crumbles when struck",
          "[game][enemies][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(sampleLevel("generators-field")));
    Enemies enemies;
    EnemyScales scales;
    scales.health = 0.75f;
    enemies.open(device, root, nullptr, 13, scales, 3);
    Generators generators;
    GeneratorScales generatorScales;
    generatorScales.health = 0.75f;
    generatorScales.most = 0.75f;
    generatorScales.rate = 1.5f;
    // The strength-three one is for a party of two; the boss generator is no kind.
    REQUIRE(generators.bind(device, layout, enemies, nullptr, generatorScales, 1));
    REQUIRE(generators.count() == 3);
    REQUIRE_FALSE(generators.standing(1));
    REQUIRE(enemies.kindLoaded(kGruntKind));
    REQUIRE(enemies.kindLoaded(kRatKind));
    // Strength two: two tiers of health at the level's three quarters, three out at once
    // (five at three quarters, made whole), a countdown of thirty (twenty at one and a half).
    const s32 chosen = 0;
    REQUIRE(generators.kindOf(chosen) == kGruntKind);
    REQUIRE(generators.tierOf(chosen) == 2);
    REQUIRE(generators.mostOf(chosen) == 3);
    REQUIRE(generators.intervalOf(chosen) == 30);
    REQUIRE(generators.healthOf(chosen) == Approx(15.0f));
    REQUIRE(generators.stateOf(chosen) == 2);
    REQUIRE(generators.standing(chosen));
    REQUIRE(generators.bodyShown(chosen));
    // The rats' one takes its defaults: ten at once, five between, at the record's scales.
    REQUIRE(generators.kindOf(2) == kRatKind);
    REQUIRE(generators.mostOf(2) == 7);
    REQUIRE(generators.intervalOf(2) == 7);
    // Nobody near: nothing is bred. A player near it: one at once, and the countdown set
    // going, stretched a little more each birth. Only the one on screen breeds.
    generators.setView(lookingAt(generators.positionOf(chosen)));
    const std::vector<EnemyView> nobody;
    generators.update(kTicks, enemies, nobody);
    REQUIRE(enemies.count() == 0);
    EnemyView near;
    near.player = 0;
    near.position = Vec3{0.0f, 0.0f, 30.0f};
    const std::vector<EnemyView> party{near};
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(chosen) == 1);
    REQUIRE(generators.bredOf(1) == 0);             // off screen
    REQUIRE(generators.countdownOf(chosen) == 180); // six ticks a unit of interval
    std::vector<s32> mine;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id) && enemies.generatorOf(id) == chosen) {
            mine.push_back(id);
        }
    }
    REQUIRE(mine.size() == 1);
    REQUIRE(enemies.kindOf(mine[0]) == kGruntKind);
    REQUIRE(enemies.tierOf(mine[0]) == 2);
    REQUIRE(enemies.algorithmOf(mine[0]) == 7);
    REQUIRE(enemies.positionOf(mine[0]).z > 4.0f); // ahead of it
    REQUIRE(glm::length(enemies.positionOf(mine[0])) == Approx(6.5f).margin(0.01f));
    // It breeds up to its count and no further while they live.
    s32 bred = 1;
    for (s32 i = 0; i < 600 && bred < 3; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
        bred = generators.bredOf(chosen);
    }
    REQUIRE(bred == 3);
    REQUIRE(generators.countdownOf(chosen) == 240); // stretched by a third
    for (s32 i = 0; i < 400; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(generators.bredOf(chosen) == 3);
    REQUIRE(enemies.count() == 3);
    // do_items leaves the timer untouched while numenemies >= maxenemies.
    REQUIRE(generators.countdownOf(chosen) == 240);
    // One killed makes room for another.
    EnemyHit slay;
    slay.damage = 100.0f;
    slay.player = 0;
    enemies.hurt(mine[0], slay);
    REQUIRE(enemies.dying(mine[0]));
    // enemy_dies uncouples immediately: the corpse does not hold a brood slot, so the
    // frozen 240-tick countdown can resume while its death animation is still present.
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(chosen) == 3);
    REQUIRE(generators.countdownOf(chosen) == 240 - kTicks);
    REQUIRE(enemies.dying(mine[0]));
    generators.update(240 - kTicks, enemies, party);
    REQUIRE(generators.countdownOf(chosen) == 0);
    REQUIRE(generators.bredOf(chosen) == 3); // Reaching zero still returns from generate_now.
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(chosen) == 4);
    REQUIRE(enemies.dying(mine[0]));
    // Struck: three of armour come off each blow, a point always getting through. It stands
    // in the state of its strength, two, until it is down to one of its record's health; it
    // then crumbles to one, breeding at strength one and twice as many (fn_8005C1DC), and
    // then it is gone, breeding no more.
    auto event = generators.strike(chosen, 2.0f, 0);
    REQUIRE(generators.healthOf(chosen) == Approx(14.0f));
    REQUIRE(event.has_value());
    REQUIRE(event->state == 2);
    REQUIRE_FALSE(event->stateChanged);
    REQUIRE_FALSE(event->destroyed);
    REQUIRE(event->kind == kGruntKind);
    REQUIRE(event->generator == chosen);
    REQUIRE(generators.stateOf(chosen) == 2);
    event = generators.strike(chosen, 4.0f, 0);
    REQUIRE(event);
    REQUIRE_FALSE(event->stateChanged);
    event = generators.strike(chosen, 8.0f, 0);
    REQUIRE(event);
    REQUIRE_FALSE(event->stateChanged);
    REQUIRE(generators.healthOf(chosen) == Approx(8.0f));
    event = generators.strike(chosen, 3.0f, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->state == 1);
    REQUIRE(event->stateChanged);
    REQUIRE(generators.tierOf(chosen) == 1);
    REQUIRE(generators.mostOf(chosen) == 6);
    event = generators.strike(chosen, 50.0f, -1);
    REQUIRE(event.has_value());
    REQUIRE(event->destroyed);
    REQUIRE_FALSE(generators.standing(chosen));
    REQUIRE(generators.obstacles().size() == 1);
    REQUIRE_FALSE(generators.strike(chosen, 50.0f, 0).has_value());
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id)) {
            enemies.hurt(id, slay);
        }
    }
    for (s32 i = 0; i < 300; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(generators.bredOf(chosen) == 4);
    REQUIRE(enemies.count() == 0);
}

TEST_CASE("a generator stands in its strength's state and a crumble doubles its brood",
          "[game][enemies][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto dir = test::scratchDirectory("generators-crumble");
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 3, "name": "GRU", "radius": 2, "height": 5, "collisionType": 1, "hitPoints": 10,
     "armor": 1}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [3, 0, 30, 0, 4, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "position": [60, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0]}
  ]
})");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    Enemies enemies;
    enemies.open(device, root, nullptr, 13, EnemyScales{}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, GeneratorScales{}, 1));
    REQUIRE(generators.count() == 2);
    // Strength three: three records of health, the third state, a caster's way.
    REQUIRE(generators.stateOf(0) == 3);
    REQUIRE(generators.tierOf(0) == 3);
    REQUIRE(generators.algorithmOf(0) == 30);
    REQUIRE(generators.mostOf(0) == 4);
    REQUIRE(generators.healthOf(0) == Approx(30.0f));
    // Down to two records it crumbles: strength two, eight at once, and a seeker's way.
    auto event = generators.strike(0, 12.0f, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->stateChanged);
    REQUIRE(event->state == 2);
    REQUIRE(generators.tierOf(0) == 2);
    REQUIRE(generators.mostOf(0) == 8);
    REQUIRE(generators.algorithmOf(0) == 0);
    // Strength one looks one state from gone, and a blow short of that is no crumble.
    REQUIRE(generators.stateOf(1) == 1);
    REQUIRE(generators.bodyShown(1));
    event = generators.strike(1, 5.0f, 0);
    REQUIRE(event.has_value());
    REQUIRE_FALSE(event->stateChanged);
    REQUIRE(generators.mostOf(1) == Generators::kDefaultMost[0]);
    event = generators.strike(1, 50.0f, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->destroyed);
    REQUIRE(generators.mostOf(1) == Generators::kDefaultMost[0]); // gone, nothing doubles
}

TEST_CASE("a generator's record gives its strength, way, count and interval, or their defaults",
          "[game][enemies]") {
    ItemInstance instance;
    instance.params = {2, 0, 7, 0, 5, 0, 20, 0, 0, 0, 0, 0};
    REQUIRE(Generators::paramOf(instance, 0) == 2);
    REQUIRE(Generators::paramOf(instance, 1) == 7);
    REQUIRE(Generators::paramOf(instance, 2) == 5);
    REQUIRE(Generators::paramOf(instance, 3) == 20);
    REQUIRE(Generators::paramOf(instance, 6) == 0);
    instance.params = {1, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(Generators::paramOf(instance, 1) == -1);
    REQUIRE(Generators::kDefaultMost[0] == 10);
    REQUIRE(Generators::kDefaultInterval[2] == 15);
}

TEST_CASE("a player's blow on a generator earns five times its kind's row of the tables",
          "[game][enemies][generators]") {
    CHECK(generatorExperience(0, false) == 5);
    CHECK(generatorExperience(2, true) == 15);
    CHECK(generatorExperience(27, false) == 5); // lbl_8011BB20
    CHECK(generatorExperience(27, true) == 75); // lbl_8011BBA8
    CHECK(generatorExperience(32, true) == 1500);
    CHECK(generatorExperience(-2, true) == 10);  // the unknown kinds count as the second
    CHECK(generatorExperience(-3, false) == 15); // and the third
    CHECK(generatorExperience(-7, true) == 5);   // any other below nought as the first
    CHECK(generatorExperience(34, true) == 0);
}

TEST_CASE("worm pits allow walking and birth at their centre without losing their target body",
          "[game][generators][enemy-collision][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/WRM/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(sampleLevel("worm-generator")));
    Enemies enemies;
    enemies.open(device, root, nullptr, 8, {}, 3);
    const std::array roster{LevelEnemy{kWormKind, kMediumClass, {}}};
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, roster));
    REQUIRE(generators.kindOf(0) == kWormKind);
    REQUIRE(generators.obstacles().size() == 2);
    REQUIRE(generators.enemyObstacles().size() == 1);
    REQUIRE(generators.struckBy({-8, 2, 0}, {8, 2, 0}, 0.5f) == 0);
    const std::array party{EnemyView{.position = Vec3{0, 0, 30}}};
    generators.setView(lookingAt(generators.positionOf(0)));
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    REQUIRE(enemies.generatorOf(0) == 0);
    CHECK(enemies.positionOf(0) == generators.positionOf(0));
    Enemies walkers;
    walkers.open(device, root, nullptr, 1, {}, 7);
    REQUIRE(walkers.loadKind(kGruntKind));
    const auto walker = walkers.spawn({.position = Vec3{0, 0, -8}, .placed = true}, {});
    REQUIRE(walker);
    const std::array destination{EnemyView{.player = 0, .position = Vec3{0, 0, 8}}};
    const auto bodies = generators.enemyObstacles();
    for (s32 frame = 0; frame < 180; ++frame) {
        walkers.update(kTicks, kStep, destination, bodies);
    }
    CHECK(walkers.positionOf(*walker).z > 0);
}

TEST_CASE("Temple generators can breed at every native collision placement",
          "[game][generators][temple-births][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level());
    GeneratorScales scales;
    scales.health = world.level()->tuning.generatorHealth;
    scales.rate = world.level()->tuning.generatorRateScale(1);
    scales.most = world.level()->tuning.generatorMostScale(1);
    usize checked = 0;
    for (s32 chosen = 0;; ++chosen) {
        // Independent broods keep earlier births from occupying a later test's exit.
        Enemies enemies;
        enemies.open(device, root, &world.collision(), world.level()->maxEnemies, {}, 7);
        Generators generators;
        REQUIRE(generators.bind(device, world.layout(), enemies, &world.collision(), scales, 1,
                                world.level()->enemies, 5, &world.items()));
        if (static_cast<usize>(chosen) >= generators.count()) {
            break;
        }
        if (!generators.standing(chosen)) {
            continue; // Authored multiplayer-only records remain dormant for this solo run.
        }
        const Vec3 at = generators.positionOf(chosen);
        CAPTURE(chosen, at.x, at.y, at.z);
        // Look down on this placement; a horizontal view would admit distant rooms
        // and legitimately fill the level-wide cap before visiting later records.
        ViewVolume view;
        view.position = at + Vec3{0, 20, 0};
        view.forward = Vec3{0, -1, 0};
        view.up = Vec3{0, 0, 1};
        generators.setView(view);
        const std::array party{EnemyView{.position = at + Vec3{0, 0, 20}}};
        generators.update(kTicks, enemies, party);
        CAPTURE(enemies.count(), world.level()->maxEnemies, generators.kindOf(chosen),
                generators.tierOf(chosen), generators.mostOf(chosen));
        CHECK(generators.bredOf(chosen) == 1);
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("Desert C1 births retain the roster and a clear path from each authored generator",
          "[game][generators][desert-births][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    test::assetOrSkip("WDATA/DESERT.WAD");
    REQUIRE(catalog.load(root));
    LevelWorld world;
    const auto level = catalog.byName("C1");
    REQUIRE(level);
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level());
    Enemies enemies;
    enemies.open(device, root, &world.collision(), Enemies::kMost, {}, 7);
    Generators generators;
    REQUIRE(generators.bind(device, world.layout(), enemies, &world.collision(), {}, 1,
                            world.level()->enemies));
    REQUIRE(generators.count() > 20);
    usize low = 0;
    for (s32 i = 0; i < static_cast<s32>(generators.count()); ++i) {
        if (generators.kindOf(i) == 6) {
            ++low;
            CHECK(generators.boxOf(i).height == 2.5f);
            CHECK(generators.boxOf(i).cylinderRadius == 2);
        }
    }
    REQUIRE(low > 0);
    REQUIRE(world.startPoint(0));
    const std::array party{EnemyView{.position = world.startPoint(0)->position}};
    generators.update(kTicks, enemies, party);
    REQUIRE(enemies.count() > 5);
    for (s32 i = 0; i < Enemies::kMost; ++i) {
        if (!enemies.alive(i)) {
            continue;
        }
        const Vec3 from = generators.positionOf(enemies.generatorOf(i));
        const Vec3 to = enemies.positionOf(i);
        CAPTURE(i, from.x, from.y, from.z, to.x, to.y, to.z);
        CHECK((enemies.kindOf(i) == 7 || enemies.kindOf(i) == 6));
        const Vec3 stopped = world.collision().sweepWalls(
            from, to, enemies.radiusOf(i), to.y + 0.1f, to.y + enemies.heightOf(i) - 0.1f);
        CHECK(glm::distance(stopped, to) < 0.01f);
    }
    // These snake pits are not the worm-generator exception.
    CHECK(generators.enemyObstacles().size() == generators.obstacles().size());
}
} // namespace
