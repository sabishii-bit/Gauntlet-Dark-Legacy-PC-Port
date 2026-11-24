#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/ApproximateLength.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "formats/CritterWad.h"
#include "game/audio/LevelSoundscape.h"
#include "game/bosses/Bosses.h"
#include "game/combat/EffectFlags.h"
#include "game/combat/HurtKind.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantFixture.h"
#include "game/critters/CritterData.h"
#include "game/critters/Critters.h"
#include "game/critters/Gargoyle.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/level/CombatantObstacle.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path familyAssets(s32 readyInterrupt = 60, u32 shield = 0) {
    const auto root = test::scratchDirectory("combatant-families");
    std::filesystem::create_directories(root / "critter");
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition(),
          bossDefinition("DJINN"), bossDefinition("LICH")}) {
        auto archive = root / "MONSTERS" / definition.name;
        if (definition.realmCostume) {
            archive /= "LEVELG";
        }
        std::filesystem::create_directories(archive / "models");
        std::filesystem::create_directories(archive / "textures");
        writeTextFile(archive / "models/body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(archive / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1}]})");
        writeFile(archive / "textures/skin.png", test::kTinyPng);
        writeTextFile(archive / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
        test::convertModelFixture(archive);
        writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"STEP","frames":3}]}]})");
        const std::string header = R"({"descriptors":[{"prefix":"BODY","name":")" +
                                   definition.name + R"(","type":)" +
                                   std::to_string(static_cast<s32>(definition.kind)) + "}],";
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      header + R"(
          "types":[{"moveCount":5,"maxHealth":100,"radius":1,"expValue":50,"shieldFlags":)" +
                          std::to_string(shield) + R"(}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":)" +
                          std::to_string(readyInterrupt) + R"(},
                   {"name":"WALK","anim":"STEP","type":52,"priority":10,"speed":3},
                   {"name":"ATTACK","anim":"STEP","type":128,"priority":20,
                    "target":{"maxDistance":5}},
                   {"name":"KD","anim":"STEP","type":66,"priority":100},
                   {"name":"DEATH","anim":"STEP","type":17,"priority":999}]})");
    }
    return root;
}

TEST_CASE("generals follow descending floors independently of combat and camera holds",
          "[combatant][critter-platform]") {
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/GENERAL/LEVELG/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":600}]}]})");
    test::FakeRenderDevice device;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    floor.object = 0;
    WorldCollision collision;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, Mat4{1});
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    const Vec3 spawn{2, 0, 0};
    REQUIRE(actor.spawn(assets, 0, spawn, 0, &collision, {}, 'G'));
    std::array players{EnemyView{.player = 0, .position = Vec3{2, 0, 3}}};
    bool timeStopped = false;
    bool cutscene = false;
    SECTION("idle") {
        actor.hold(true);
    }
    SECTION("attacking") {
        actor.update(2, 1.0f / 30, players);
        REQUIRE(actor.moveType() == 128);
    }
    SECTION("frozen") {
        actor.freeze(600);
    }
    SECTION("time stopped") {
        timeStopped = true;
    }
    SECTION("cutscene without combat updates") {
        cutscene = true;
    }
    const f32 health = actor.health();
    const std::string move{actor.moveName()};
    for (s32 frame = 1; frame <= 60; ++frame) {
        const auto elapsed = static_cast<f32>(frame);
        const Mat4 platform = glm::translate(Mat4{1}, Vec3{0, -0.5f * elapsed, 0}) *
                              glm::rotate(Mat4{1}, elapsed * 0.01f, Vec3{0, 1, 0});
        collision.setObjectTransform(0, platform);
        players[0].position = Vec3{platform * Vec4{Vec3{2, 0, 3}, 1}};
        if (cutscene) {
            actor.syncFloor();
            CHECK(actor.moveName() == move);
            CHECK(actor.takeBlows().empty());
            CHECK(actor.takeCues().empty());
        } else {
            actor.update(2, 1.0f / 30, players, {}, timeStopped);
        }
        const Vec3 expected{platform * Vec4{spawn, 1}};
        CHECK(glm::distance(actor.position(), expected) < 0.001f);
        actor.syncFloor();
        CHECK(glm::distance(actor.position(), expected) < 0.001f);
        CHECK(actor.health() == health);
    }
    // Removed support must not drag a body to a now-nonsolid platform's next position.
    const Vec3 before = actor.position();
    collision.setSolid(0, false);
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -60, 0}));
    actor.syncFloor();
    CHECK(actor.position() == before);
}

TEST_CASE("combatant visual frames interpolate without moving hit nodes or advancing moves",
          "[combatant][presentation]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    TreeInfo tree = *assets.tree;
    auto& sequence = tree.sequences[0];
    sequence.frames = 600;
    TrackInfo track;
    track.node = 0;
    track.flags = TrackInfo::channelBit(3);
    track.frames = {0, 599};
    track.values = {0, 599};
    sequence.tracks = {track};
    sequence.trackOfNode = {0};
    assets.tree = &tree;
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, nullptr, {}, 'G'));
    actor.update(1, 1.0f / 60, {});
    const auto hitNode = actor.nodeTransform("BODY");
    REQUIRE(hitNode);
    CHECK((*hitNode)[3].x == Approx(1));
    const auto vertex = [&](f32 alpha) {
        device.draws.clear();
        actor.draw(device, Mat4{1}, {}, nullptr, nullptr, nullptr, alpha);
        REQUIRE_FALSE(device.draws.empty());
        return device.draws[0].vertices[0].position;
    };
    const Vec3 beginning = vertex(0);
    for (const f32 alpha : {0.25f, 0.5f, 0.75f, 1.0f}) {
        CHECK(vertex(alpha).x - beginning.x == Approx(0.5f * alpha));
        CHECK(actor.nodeTransform("BODY") == hitNode);
        CHECK(actor.moveName() == "READY");
        CHECK(actor.takeBlows().empty());
        CHECK(actor.takeShots().empty());
    }
    // Freeze and stopped-time early returns cannot keep replaying the last interval.
    SECTION("legend freeze") {
        actor.freeze(60);
        actor.update(1, 1.0f / 60, {});
        CHECK(vertex(0) == vertex(1));
    }
    SECTION("stopped time") {
        actor.update(1, 1.0f / 60, {}, {}, true);
        CHECK(vertex(0) == vertex(1));
    }
    SECTION("new slot occupant") {
        REQUIRE(actor.spawn(assets, 0, Vec3{100, 0, 0}, 0, nullptr, {}, 'G'));
        CHECK(vertex(0) == vertex(1));
        CHECK(vertex(0).x == Approx(100));
    }
    assets.tree = nullptr; // The synthetic override dies before the owning archive.
}

TEST_CASE("the ordinary creature roster synchronizes floors without a gameplay tick",
          "[combatant][critter-platform]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    floor.object = 0;
    WorldCollision collision;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, Mat4{1});
    Critters critters;
    critters.open(device, root, &collision, {}, 'G');
    const std::array ids{critters.spawnGeneral(Vec3{0}, 0), critters.spawnGolem(Vec3{10, 0, 0}, 0),
                         critters.spawnGargoyle(Vec3{-10, 0, 0}, 0, "GAR_EAGL")};
    for (const auto id : ids) {
        REQUIRE(id);
    }
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -20, 0}));
    critters.update(0, 0, {});
    for (const auto id : ids) {
        CHECK(critters.positionOf(*id).y == Approx(-20));
        CHECK(critters.moveOf(*id) == "READY");
    }
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -30, 0}));
    critters.syncFloors();
    for (const auto id : ids) {
        CHECK(critters.positionOf(*id).y == Approx(-30));
        CHECK(critters.moveOf(*id) == "READY");
    }
}

TEST_CASE("a walking general retains its new position on a moving floor",
          "[combatant][critter-platform]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    floor.object = 0;
    WorldCollision collision;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, Mat4{1});
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, &collision, {}, 'G'));
    const std::array players{EnemyView{.player = 0, .position = Vec3{0, 0, 20}}};
    for (s32 frame = 0; frame < 30; ++frame) {
        actor.update(2, 1.0f / 30, players);
    }
    REQUIRE(actor.position().z > 1);
    const Vec3 walked = actor.position();
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -10, 0}));
    actor.syncFloor();
    CHECK(actor.position().x == Approx(walked.x));
    CHECK(actor.position().z == Approx(walked.z));
    CHECK(actor.position().y == Approx(-10));
}

TEST_CASE("a charging general checks the floor ahead before stepping off a deck",
          "[combatant][critter-traversal]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":3.5,"wallRadius":2,
                "originOffset":[0,3,0]}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"CHARGE","anim":"STEP","type":134,"speed":15,
                "priority":20,"target":{"minDistance":10}}]})");
    std::vector<CollisionTriangle> triangles;
    const auto plane = [&](f32 start, f32 end, f32 height) {
        CollisionTriangle first;
        first.vertices = {Vec3{-20, height, start}, Vec3{20, height, end}, Vec3{20, height, start}};
        CollisionTriangle second;
        second.vertices = {Vec3{-20, height, start}, Vec3{-20, height, end}, Vec3{20, height, end}};
        triangles.push_back(first);
        triangles.push_back(second);
    };
    plane(-20, 5, 0);
    std::optional<f32> stepHeight;
    SECTION("empty space beyond the deck") {}
    SECTION("lower hull surface is too far down") {
        plane(5, 30, -5.5f);
    }
    SECTION("ordinary descending steps remain traversable") {
        plane(5, 30, -1);
        stepHeight = -1.0f;
    }
    SECTION("ordinary ascending steps remain traversable") {
        plane(5, 30, 1);
        stepHeight = 1.0f;
    }
    WorldCollision collision;
    collision.build(triangles);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, &collision, {}, 'G'));
    const std::array players{EnemyView{.player = 0, .position = Vec3{0, 0, 25}}};
    bool charged = false;
    for (s32 tick = 0; tick < 120; ++tick) {
        actor.update(1, 1.0f / 60, players);
        charged = charged || actor.moveName() == "CHARGE";
        if (!stepHeight) {
            REQUIRE(actor.position().y == Approx(0));
            REQUIRE(actor.position().z <= 3.001f);
        }
    }
    REQUIRE(charged);
    CHECK(actor.position().z > 1);
    if (stepHeight) {
        CHECK(actor.position().z > 6);
        CHECK(actor.position().y == Approx(*stepHeight));
    }
}

TEST_CASE("Sky Dominion generals retain forward support during charges and melee knockback",
          "[combatant][critter-traversal][assets]") {
    const usize instanceIndex = GENERATE(119U, 189U);
    const s32 octant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(instanceIndex, octant);
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("K1")));
    const auto& instance = world.layout().itemInstances()[instanceIndex];
    REQUIRE(world.layout().itemInfos()[static_cast<usize>(instance.info)].name == "GENERAL");
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'K'));
    const f32 yaw = static_cast<f32>(octant) * glm::quarter_pi<f32>();
    const Vec3 direction{std::sin(yaw), 0, std::cos(yaw)};
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, instance.position, yaw, &world.collision(), {}, 'K'));
    const Vec3 start = actor.position();
    bool charged = false;
    f32 travelled = 0;
    for (s32 frame = 0; frame < 300; ++frame) {
        const Vec3 before = actor.position();
        const std::array players{EnemyView{.player = 0, .position = before + direction * 20.0f}};
        if (frame == 150) {
            // The same world check must constrain a shove as well as locomotion.
            EnemyHit hit;
            hit.damage = 3;
            hit.flags = EnemyHit::kKnockBack;
            hit.direction = glm::normalize(direction + Vec3{0, 1, 0});
            hit.where = before;
            actor.hurt(hit);
        }
        actor.update(2, 1.0f / 30, players);
        charged = charged || actor.moveName() == "CHARGE";
        const Vec3 delta = actor.position() - before;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        travelled += distance;
        if (distance > 0.001f) {
            const Vec3 ahead = actor.position() +
                               Vec3{delta.x, 0, delta.z} * (assets.data.wallRadius() / distance);
            const auto support = world.collision().floorAt(ahead, 2, 6);
            CAPTURE(frame, actor.position().x, actor.position().y, actor.position().z);
            REQUIRE(support);
        }
    }
    CHECK(charged);
    CHECK(travelled > 0.5f);
    CHECK(glm::distance(actor.position(), start) > 0.1f);
}

TEST_CASE("ordinary combatants rank four player slots by facing and recent hit grace",
          "[combatant][multiplayer-targeting]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,"target":{"maxDistance":100}}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    std::array<EnemyView, 4> players;
    players[0].player = 3;
    players[0].position = {0, 0, 6};
    players[1].player = 1;
    players[1].position = {0, 0, -4};
    players[2].player = 0;
    players[2].position = {0, 0, 1};
    players[2].hidden = true;
    players[3].player = 2;
    players[3].position = {0, 0, 2};
    players[3].invisible = true;
    const auto step = [&] { actor.update(2, 1.0f / 30, players); };
    step();
    CHECK(actor.target() == 3); // six ahead scores better than four behind (score eight)
    players[0].recentlyHit = true;
    step();
    CHECK(actor.target() == 1);
    players[1].invisible = true;
    step();
    CHECK(actor.target() == 3); // grace is a penalty, not immunity from being targeted
    players[3].invisible = false;
    players[3].damageable = false;
    step();
    CHECK(actor.target() == 2); // combo/turbo immunity does not hide a target
    players[3].hidden = true;
    players[0].hidden = true;
    step();
    CHECK(actor.target() == -1);
}

TEST_CASE("combatant sight uses the actual bearing and keeps sparse identity ties stable",
          "[combatant][multiplayer-targeting]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,
                "target":{"maxDistance":20,"minDot":0.5,"maxVertical":5}}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    std::array<EnemyView, 2> players;
    players[0].player = 3;
    players[0].position = {0, 0, -2};
    players[1].player = 1;
    players[1].position = {0, 0, 10};
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() == 1);
    players[0].position = players[1].position;
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() == 1); // retail scans input slots, not the party's vector order
    players[1].position.y = 6;
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() == 3);
    players[0].position.z = 21;
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() == -1);
}

TEST_CASE("boss anger remembers each player's damage exchange for fifteen seconds",
          "[combatant][multiplayer-targeting]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    std::array<EnemyView, 2> players;
    players[0].player = 3;
    players[0].position = {0, 0, 4};
    players[1].player = 1;
    players[1].position = {0, 0, 10};
    players[1].invisible = true; // bosses see invisibility; ordinary critters do not
    const auto step = [&](f32 seconds = 0.25f) { actor.update(1, seconds, players); };
    step();
    REQUIRE(actor.target() == 3);
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 10;
    actor.hurt(hit);
    step();
    CHECK(actor.target() == 1);
    actor.damagedPlayer(1, 100);
    step();
    CHECK(actor.target() == 3); // inverse anger caps at ten after the boss strikes back
    step(14.25f);
    CHECK(actor.target() == 3);
    actor.hurt(hit); // dealt refreshed independently; received still remembered
    step();
    CHECK(actor.target() == 3);
    step(); // received is exactly fifteen seconds old: still present
    CHECK(actor.target() == 3);
    step();
    CHECK(actor.target() == 1); // received expires, but the newer dealt ledger remains
    step(15.0f);
    CHECK(actor.target() == 3); // both ledgers have now expired
    actor.hurt(hit);
    step();
    REQUIRE(actor.target() == 1);
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    step();
    CHECK(actor.target() == 3); // no ledger leaks across slot reuse
    players[0].hidden = true;
    players[1].hidden = true;
    step();
    CHECK(actor.target() == -1);
}

TEST_CASE("combatant sight measures player effect height from its elevated targeting centre",
          "[combatant][multiplayer-targeting][combatant-target-origin]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":100,"floorOffset":5,
                "originOffset":[0,7,0],"target":{"maxVertical":2}}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 2;
    player.height = 100; // Visual height must not replace the native effect-position offset.
    player.collisionHeight = 3.0f;
    player.position = {0, 9, 10};
    bool expected = true;
    SECTION("centre") {}
    SECTION("upper inclusive boundary") {
        player.position.y = 11;
    }
    SECTION("lower inclusive boundary") {
        player.position.y = 7;
    }
    SECTION("above the upper boundary") {
        player.position.y = 11.01f;
        expected = false;
    }
    SECTION("below the lower boundary") {
        player.position.y = 6.99f;
        expected = false;
    }
    SECTION("height fallback for a view without a native collision centre") {
        player.height = 6;
        player.collisionHeight.reset();
    }
    actor.update(2, 1.0f / 30, std::array{player});
    CHECK(actor.target() == (expected ? 2 : -1));
}

TEST_CASE("combatant sight rotates an offset origin before testing distance and facing",
          "[combatant][multiplayer-targeting][combatant-target-origin]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":100,"originOffset":[0,3,5],
                "target":{"maxDistance":10,"minDot":0.5}}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    constexpr f32 kQuarterTurn = 1.5707963267948966f;
    REQUIRE(actor.spawn(assets, 0, {}, kQuarterTurn, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 2;
    player.position = {13, 0, 0};
    bool expected = true;
    SECTION("inside the shifted distance window") {}
    SECTION("past the shifted distance window") {
        player.position.x = 15.01f;
        expected = false;
    }
    SECTION("ahead of the feet but behind the targeting centre") {
        player.position.x = 2;
        expected = false;
    }
    actor.update(2, 1.0f / 30, std::array{player});
    CHECK(actor.target() == (expected ? 2 : -1));
}

TEST_CASE("ready movement checks effect-position height rather than floor height",
          "[combatant][combatant-target-origin][critter-ready-selection]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"originOffset":[0,10,0]}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":90},
               {"name":"STEP","anim":"STEP","type":52,
                "target":{"maxVertical":1}}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 2;
    player.position = {0, 8, 10};
    bool expected = true;
    SECTION("upper inclusive boundary") {}
    SECTION("lower inclusive boundary") {
        player.position.y = 6;
    }
    SECTION("above the upper boundary") {
        player.position.y = 8.01f;
        expected = false;
    }
    SECTION("below the lower boundary") {
        player.position.y = 5.99f;
        expected = false;
    }
    actor.update(2, 1.0f / 30, std::array{player});
    CHECK(actor.moveName() == (expected ? "STEP" : "READY"));
}

TEST_CASE("child target sight inherits the parent's targeting origin rather than its own mesh",
          "[combatant][multiplayer-targeting][combatant-target-origin]") {
    const auto root = test::headedBodyAssets();
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000,"childIndex":1,
                "originOffset":[0,8,5],
                "target":{"maxDistance":10,"minDot":0.5,"maxVertical":1}},
               {"rootNode":"HEAD_L","moveCount":1,"maxHealth":100,"parentIndex":0,
                "childIndex":-1,"floorOffset":50,"originOffset":[100,100,100],
                "target":{"maxDistance":10,"minDot":0.5,"maxVertical":1}}],
      "moves":[{"name":"READY","anim":"READY","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    constexpr f32 kQuarterTurn = 1.5707963267948966f;
    REQUIRE(actor.spawn(assets, 40, {}, kQuarterTurn, nullptr, {}, 'A'));
    EnemyView player;
    player.player = 2;
    player.position = {10, 5, 0};
    actor.update(2, 1.0f / 30, std::array{player});
    REQUIRE(actor.child(41));
    CHECK(actor.target() == 2);
    CHECK(actor.child(41)->target() == 2);
}

TEST_CASE("child attack selection retains the target geometry sampled before its parent moves",
          "[combatant][multiplayer-targeting][combatant-target-origin]") {
    const auto root = test::headedBodyAssets();
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000,"childIndex":1,"roamRadius":100},
               {"rootNode":"HEAD_L","moveIndex":1,"moveCount":2,"maxHealth":100,
                "parentIndex":0,"childIndex":-1}],
      "moves":[{"name":"READY","anim":"READY","type":32,"speed":60},
               {"name":"READY","anim":"READY","type":32,"interrupt":90},
               {"name":"ATTACK","anim":"SPIT","type":128,
                "target":{"minDistance":10,"minDot":0.88}}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'A'));
    EnemyView player;
    player.player = 2;
    player.position = {5, 0, 10};
    actor.update(2, 1.0f / 30, std::array{player});
    REQUIRE(actor.child(41));
    REQUIRE(actor.position().z == Approx(2));
    CHECK(actor.child(41)->moveName() == "ATTACK");
}

TEST_CASE("child attack selection uses the family's facing before the parent turns",
          "[combatant][multiplayer-targeting][combatant-target-origin]") {
    const auto root = test::headedBodyAssets();
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000,"childIndex":1},
               {"rootNode":"HEAD_L","moveIndex":1,"moveCount":2,"maxHealth":100,
                "parentIndex":0,"childIndex":-1}],
      "moves":[{"name":"READY","anim":"READY","type":32,"turnRate":30},
               {"name":"READY","anim":"READY","type":32,"interrupt":90},
               {"name":"ATTACK","anim":"SPIT","type":128,
                "target":{"minDot":0.95}}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'A'));
    EnemyView player;
    player.player = 2;
    player.position = {5, 0, 10};
    actor.update(2, 1.0f / 30, std::array{player});
    REQUIRE(actor.child(41));
    REQUIRE(actor.yaw() == Approx(std::atan2(5.0f, 10.0f)));
    // BossAI selects the child before CritterRotate. Turning into the cone
    // makes it eligible next update, not retroactively in this one.
    CHECK(actor.child(41)->moveName() == "READY");
    actor.update(2, 1.0f / 30, std::array{player});
    CHECK(actor.child(41)->moveName() == "ATTACK");
}

TEST_CASE("boss move selection uses the anger-ranked eligible roster",
          "[combatant][multiplayer-targeting]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":2,"maxHealth":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
        {"name":"ATTACK","anim":"STEP","type":128,"priority":20,
         "target":{"minDistance":10}}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    std::array<EnemyView, 4> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].player = static_cast<s32>(players.size() - i - 1);
        players[i].position = {0, 0, 5.0f + 5.0f * static_cast<f32>(i)};
    }
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 5;
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, players);
    REQUIRE(actor.moveName() == "ATTACK");
    CHECK(actor.target() == 0); // farthest, but the only one who has hurt the boss
    players[3].hidden = true;
    actor.update(60, 1.0f, players);
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() != 0);
}

TEST_CASE("combatant volleys keep their move target while the next attack selects afresh",
          "[combatant][multiplayer-targeting][combatant-target-lock]") {
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/LICH/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":2},{"name":"VOLLEY","frames":12}]}]})");
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":2,"maxHealth":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
        {"name":"VOLLEY","anim":"VOLLEY","type":133,"priority":20,
         "frameStart":1,"frameEnd":9,"framePeriod":4,"damage0":0}],
      "damages":[{"type":1,"damage":10,"minSpeed":20}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    CHECK(assets.data.rules().projectileTargetHeight < 0);
    CHECK_FALSE(assets.data.rules().restoreCaptureSupport);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
    std::array<EnemyView, 4> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].player = static_cast<s32>(players.size() - i - 1);
        players[i].position = {0, 0, 20.0f + 10.0f * static_cast<f32>(i)};
    }
    actor.update(2, 1.0f / 30, players);
    REQUIRE(actor.moveName() == "VOLLEY");
    auto shots = actor.takeShots();
    REQUIRE(shots.size() == 1);
    const auto centre = [](const EnemyView& view) {
        return view.position + Vec3{0, view.height * 0.5f, 0};
    };
    CHECK(shots.front().target == centre(players[0]));
    // Candidate 1 becomes closer and angrier, but the active move still owns slot 3.
    players[2].position = {1, 0, 2};
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 5;
    actor.hurt(hit);
    players[0].position = {5, 0, 25};
    actor.update(2, 4.0f / 30, players);
    CHECK(actor.target() == 1);
    shots = actor.takeShots();
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().target == centre(players[0])); // tracks the same player's current point
    SECTION("hidden former target retains the retail in-flight identity") {
        players[0].hidden = true;
        actor.update(2, 4.0f / 30, players);
        shots = actor.takeShots();
        REQUIRE(shots.size() == 1);
        CHECK(shots.front().target == centre(players[0]));
    }
    SECTION("missing former slot never aliases the remaining view index") {
        actor.update(2, 4.0f / 30, std::span<const EnemyView>{players}.subspan(1));
        shots = actor.takeShots();
        REQUIRE(shots.size() == 1);
        CHECK_FALSE(shots.front().target.has_value());
    }
    actor.update(2, 3.0f / 30, players); // finish the volley
    actor.takeShots();
    actor.update(2, 2.0f / 30, players); // finish READY
    actor.update(2, 1.0f / 30, players); // the next volley takes the new candidate
    REQUIRE(actor.moveName() == "VOLLEY");
    shots = actor.takeShots();
    REQUIRE(shots.size() == 1);
    CHECK(shots.front().target == centre(players[2]));
}

TEST_CASE("ordinary combatants schedule attacks before steps and alternate eligible moves",
          "[combatant][multiplayer-targeting][ordinary-attack-selection]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition()}) {
        CAPTURE(definition.name);
        auto archive = root / "MONSTERS" / definition.name;
        if (definition.realmCostume) {
            archive /= "LEVELG";
        }
        writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"STEP","frames":3},{"name":"ATTACK","frames":12}]}]})");
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      R"({"descriptors":[{"prefix":"BODY","name":")" + definition.name +
                          R"(","type":)" + std::to_string(static_cast<s32>(definition.kind)) +
                          R"(}],"types":[{"moveCount":4,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
            {"name":"HIGH","anim":"ATTACK","type":132,"priority":100,"cooldown":1,
             "target":{"maxDistance":10},"frameStart":1,"damage0":0},
            {"name":"LOW","anim":"ATTACK","type":132,"priority":10,
             "target":{"maxDistance":10},"frameStart":1,"damage0":0},
            {"name":"WALK","anim":"STEP","type":52,"priority":1000,"interrupt":60}],
          "damages":[{"type":1,"damage":10,"minSpeed":20}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        for (const usize count : {2U, 4U}) {
            CAPTURE(count);
            Combatant actor;
            REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
            std::array<EnemyView, 4> players;
            for (usize i = 0; i < players.size(); ++i) {
                players[i].player = static_cast<s32>(players.size() - i - 1);
                players[i].position = {0, 0, 6.0f + 2.0f * static_cast<f32>(i)};
            }
            // Two players use sparse identities 3 and 1, not vector indices.
            std::swap(players[1].player, players[2].player);
            const auto party = std::span<const EnemyView>{players}.first(count);
            actor.update(2, 1.0f / 30, party);
            REQUIRE(actor.moveName() == "HIGH"); // a valid attack precedes even priority-1000 WALK
            auto shots = actor.takeShots();
            REQUIRE(shots.size() == 1);
            CHECK(shots.front().target == players[0].position + Vec3{0, players[0].height / 2, 0});
            players[1].position = {0, 0, 2};
            actor.update(2, 4.0f / 30, party);
            CHECK(actor.moveName() == "HIGH"); // LOW cannot interrupt HIGH's locked animation
            actor.update(2, 8.0f / 30, party);
            REQUIRE(actor.moveDone());
            actor.update(2, 1.0f / 30, party);
            REQUIRE(actor.moveName() == "LOW");
            CHECK(actor.target() == 1);
            shots = actor.takeShots();
            REQUIRE(shots.size() == 1);
            CHECK(shots.front().target == players[1].position + Vec3{0, players[1].height / 2, 0});
            actor.update(2, 12.0f / 30, party);
            REQUIRE(actor.moveDone());
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "WALK"); // HIGH cooling; current LOW cannot select itself
            for (s32 frame = 0; frame < 45 && actor.moveName() != "HIGH"; ++frame) {
                actor.update(2, 1.0f / 30, party);
            }
            CHECK(actor.moveName() == "HIGH"); // HIGH is now the oldest eligible move
            for (EnemyView& player : players) {
                player.hidden = true;
            }
            actor.update(2, 12.0f / 30, party);
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "READY");
        }
    }
}

TEST_CASE("ordinary critters block only their selected player's heavy attack",
          "[combatant][multiplayer-targeting][critter-block]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition()}) {
        CAPTURE(definition.name);
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      R"({"descriptors":[{"prefix":"BODY","name":")" + definition.name +
                          R"(","type":)" + std::to_string(static_cast<s32>(definition.kind)) +
                          R"(}],"types":[{"moveCount":4,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
            {"name":"ATTACK","anim":"STEP","type":128,"interrupt":60},
            {"name":"BLOCK_FIRST","anim":"STEP","type":35,"priority":100,"cooldown":1,
             "target":{"maxDistance":10}},
            {"name":"BLOCK_LAST","anim":"STEP","type":35,"priority":10,"cooldown":1,
             "target":{"maxDistance":10}}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        for (const usize count : {2U, 4U}) {
            CAPTURE(count);
            Combatant actor;
            REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
            std::array<EnemyView, 4> players;
            for (usize i = 0; i < players.size(); ++i) {
                players[i].player = static_cast<s32>(players.size() - i - 1);
                players[i].position = {0, 0, 4.0f + 4.0f * static_cast<f32>(i)};
            }
            std::swap(players[1].player, players[2].player);
            const auto party = std::span<const EnemyView>{players}.first(count);
            players[1].blockableAttack = true;
            actor.update(2, 1.0f / 30, party);
            REQUIRE(actor.moveName() == "ATTACK"); // not provoked by another player's move
            players[0].blockableAttack = true;
            actor.update(2, 1.0f / 30, party);
            REQUIRE(actor.moveName() == "BLOCK_LAST"); // table order, not highest priority
            EnemyHit hit;
            hit.damage = 12;
            hit.player = 3;
            hit.flags = 0x120;
            actor.hurt(hit);
            CHECK(actor.health() == Approx(997)); // block quarter and no heavy reaction
            players[0].blockableAttack = false;
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "BLOCK_LAST"); // locked animation is not interrupted
            actor.update(2, 3.0f / 30, party);
            actor.update(2, 1.0f / 30, party);
            REQUIRE(actor.moveName() == "ATTACK");
            players[0].blockableAttack = true;
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "BLOCK_FIRST"); // the later block is cooling down
            for (EnemyView& player : players) {
                player.hidden = true;
            }
            actor.update(2, 3.0f / 30, party);
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "READY");
            actor.update(60, 2.0f, party);
            players[1].hidden = false; // sparse slot 1 is now the only candidate
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.target() == 1);
            CHECK(actor.moveName() == "BLOCK_LAST");
        }
    }
}

TEST_CASE("ready steps precede idle moves and rank by their target direction",
          "[combatant][critter-ready]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition()}) {
        CAPTURE(definition.name);
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      R"({"descriptors":[{"prefix":"BODY","name":")" + definition.name +
                          R"(","type":)" + std::to_string(static_cast<s32>(definition.kind)) +
                          R"(}],"types":[{"moveCount":4,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"priority":1000,"interrupt":90},
            {"name":"OBLIQUE","anim":"STEP","type":52,"priority":900,
             "target":{"yaw":1,"minDot":0.1}},
            {"name":"FORWARD","anim":"STEP","type":52,"priority":10,
             "target":{"yaw":0,"minDot":0.1}},
            {"name":"TAUNT","anim":"STEP","type":33,"priority":2000}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        for (const f32 bearing : {0.0f, 1.0f}) {
            CAPTURE(bearing);
            Combatant actor;
            REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
            EnemyView player;
            player.player = 3;
            player.position = {20 * std::sin(bearing), 0, 20 * std::cos(bearing)};
            actor.update(2, 1.0f / 30, std::array{player});
            CHECK(actor.moveName() == (bearing == 0 ? "FORWARD" : "OBLIQUE"));
        }
    }
}

TEST_CASE("ordinary taunts obey the same health and cooldown gates as bosses",
          "[combatant][critter-ready]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":90},
               {"name":"TAUNT","anim":"STEP","type":33,"priority":1000,"cooldown":10}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
    SECTION("healthy taunts then waits out its authored cooldown") {
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.moveName() == "TAUNT");
        actor.update(2, 3.0f / 30, {});
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.moveName() == "READY");
    }
    SECTION("a wounded creature no longer taunts while idle") {
        EnemyHit hit;
        hit.damage = 100;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.moveName() == "READY");
    }
}

TEST_CASE("ready step cooldown preserves the family's animation or move-entry origin",
          "[combatant][critter-ready][move-cooldown]") {
    const bool cooldownStartsWithMove = GENERATE(false, true);
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/GENERAL/LEVELG/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":3},{"name":"WALK","frames":30}]}]})");
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":2,"maxHealth":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"priority":10},
               {"name":"WALK","anim":"WALK","type":52,"priority":20,"cooldown":1}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    REQUIRE_FALSE(assets.cooldownStartsWithMove);
    assets.cooldownStartsWithMove = cooldownStartsWithMove;
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 3;
    player.position = {0, 0, 20};
    const std::array players{player};
    actor.update(2, 3.0f / 30, players);
    actor.update(2, 1.0f / 30, players);
    REQUIRE(actor.moveName() == "WALK");
    actor.update(2, 1.0f, players);
    REQUIRE(actor.moveDone());
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.moveName() == (cooldownStartsWithMove ? "WALK" : "READY"));
    for (s32 frame = 0; frame < 35 && actor.moveName() != "WALK"; ++frame) {
        actor.update(2, 1.0f / 30, players);
    }
    CHECK(actor.moveName() == "WALK");
}

TEST_CASE("combatant immutable timing policies participate in resource identity",
          "[combatant][move-cooldown][native-state-resources]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets ordinary;
    CombatantAssets imported;
    REQUIRE(ordinary.load(device, root, General::definition(), 'G'));
    REQUIRE(imported.load(device, root, General::definition(), 'G'));
    const auto hash = [](const CombatantAssets& assets) {
        NativeStateResources resources;
        REQUIRE(resources.registerCombatantAssets(assets, "test/fighter"));
        NativeStateDigest digest(resources.resources());
        digest.resource(&assets);
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    const auto original = hash(ordinary);
    CHECK(hash(imported) == original);
    imported.cooldownStartsWithMove = true;
    CHECK(hash(imported) != original);
    imported.cooldownStartsWithMove = false;
    imported.projectileLifetime = 5;
    CHECK(hash(imported) != original);
    imported.clear();
    CHECK_FALSE(imported.cooldownStartsWithMove);
    CHECK(imported.projectileLifetime == 0);
    REQUIRE(imported.load(device, root, General::definition(), 'G'));
    CHECK(hash(imported) == original);
}

TEST_CASE("each family's combatant identity keeps its bytes",
          "[game][combatant][native-state-digest][native-state-resources][native-digest-pin]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    // SHA-256 of a digest holding only the family's registered identity, which netplay peers
    // must hash alike: a literal moves only when what the identity takes changes on purpose.
    const std::array families{
        std::pair{
            bossDefinition("LICH"),
            std::string_view{"40a15d9e20e67dc55a28cefd9327b1112ae784fb7547c926a65fff092fb5c383"}},
        std::pair{
            Golem::definition(),
            std::string_view{"ce056bafbf1f747b9f266d72f1bcfb5d4c484759d3d9745c3107fdd3d13d6af7"}},
        std::pair{
            Gargoyle::definition(),
            std::string_view{"4e38131b3b607d39f10837491f1c138dd614babbb57bd0084bedde4d37c421e3"}}};
    for (const auto& [definition, expected] : families) {
        CAPTURE(definition.name);
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        NativeStateResources resources;
        REQUIRE(resources.registerCombatantAssets(assets, "critters/" + definition.name));
        NativeStateDigest digest(resources.resources());
        digest.resource(&assets);
        const auto identity = digest.checkedFinish();
        REQUIRE(identity);
        CHECK(test::hexBytes(*identity) == expected);
    }
}

TEST_CASE("native eagle gargoyle approaches a player beyond its attack ranges",
          "[combatant][critter-ready][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GAR_EAGL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    const auto walk = assets.data.moveNamed("WALK");
    const auto ready = assets.data.moveOfType(MoveDefinition::kReady);
    REQUIRE(walk.has_value());
    REQUIRE(ready.has_value());
    REQUIRE(assets.data.moves()[*walk].priority < assets.data.moves()[*ready].priority);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 3;
    player.position = {0, 0, 60};
    for (s32 frame = 0; frame < 300 && actor.position().z < 2; ++frame) {
        actor.update(2, 1.0f / 30, std::array{player});
    }
    CHECK(actor.target() == 3);
    CHECK(actor.position().z >= 2);
}

TEST_CASE("native generals and golems select their authored block against a heavy attack",
          "[combatant][critter-block][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    for (const auto& definition : {General::definition(), Golem::definition()}) {
        CAPTURE(definition.name);
        test::assetOrSkip("MONSTERS/" + definition.name + "/LEVELG/ANIM.PS2");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        const auto index = assets.data.moveOfType(MoveDefinition::kBlock);
        REQUIRE(index.has_value());
        const MoveDefinition& block = assets.data.moves()[*index];
        REQUIRE(block.target.minDistance == 0);
        REQUIRE(block.target.maxDistance == 15);
        REQUIRE(block.target.minRateScale == 0);
        REQUIRE(block.target.maxRateScale == 0);
        Combatant actor;
        REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
        std::array<EnemyView, 2> players;
        players[0].player = 3;
        players[0].position = {8.0f * std::sin(block.target.yaw), 0,
                               8.0f * std::cos(block.target.yaw)};
        players[0].blockableAttack = true;
        players[1].player = 1;
        players[1].hidden = true;
        for (s32 frame = 0; frame < 600 && actor.moveType() != MoveDefinition::kBlock; ++frame) {
            actor.update(2, 1.0f / 30, players);
        }
        CHECK(actor.target() == 3);
        CHECK(actor.moveType() == MoveDefinition::kBlock);
        CHECK(actor.moveName() == block.name);
    }
}

TEST_CASE("ordinary defensive moves respect authored eligibility and locked animations",
          "[combatant][critter-block]") {
    struct Case {
        u32 flags;
        std::string node;
        s32 link;
        s32 interrupt;
        f32 distance;
        bool hidden;
        bool invisible;
        bool blocks;
        std::string animation = "STEP";
    };
    const std::array cases{Case{0, "", -1, 60, 4, false, false, true},
                           Case{4, "", -1, 60, 4, false, false, false},
                           Case{16, "MISSING", -1, 60, 4, false, false, false},
                           Case{16, "BODY", 2, 60, 4, false, false, false},
                           Case{0, "", -1, 0, 4, false, false, false},
                           Case{0, "", -1, 60, 20, false, false, false},
                           Case{0, "", -1, 60, 4, true, false, false},
                           Case{0, "", -1, 60, 4, false, true, false},
                           Case{0, "", -1, 60, 4, false, false, false, "MISSING"}};
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const Case& entry : cases) {
        CAPTURE(entry.flags, entry.node, entry.link, entry.interrupt, entry.distance, entry.hidden,
                entry.invisible);
        writeTextFile(root / "critter/GENERAL.json",
                      R"({
          "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
          "types":[{"moveCount":3,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":)" +
                          std::to_string(entry.interrupt) + R"(},
            {"name":"BLOCK","anim":")" +
                          entry.animation + R"(","type":35,"priority":3840,"flags":)" +
                          std::to_string(entry.flags) + R"(,"colnode":")" + entry.node +
                          R"(","link":)" + std::to_string(entry.link) + R"(,
             "target":{"maxDistance":10}},
            {"name":"LINK","anim":"STEP","type":35,"flags":4,"colnode":"MISSING"}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, General::definition(), 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'G'));
        EnemyView player;
        player.player = 3;
        player.position = {0, 0, entry.distance};
        player.blockableAttack = true;
        player.hidden = entry.hidden;
        player.invisible = entry.invisible;
        const std::array party{player};
        actor.update(2, 1.0f / 30, party);
        CHECK((actor.moveName() == "BLOCK") == entry.blocks);
        if (entry.animation == "MISSING") {
            actor.update(2, 3.0f / 30, party);
            CHECK(actor.moveDone()); // invalid defensive art must not freeze the fallback
        }
        if (entry.interrupt == 0) {
            actor.update(2, 3.0f / 30, party);
            actor.update(2, 1.0f / 30, party);
            CHECK(actor.moveName() == "BLOCK"); // completion permits the previously refused block
        }
    }
}

TEST_CASE("boss child candidate distribution removes the weakest claim and resets each frame",
          "[combatant][multiplayer-targeting]") {
    const auto root = test::headedBodyAssets();
    // Keep only READY moves so the target policy is observed without an authored pattern.
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":1,"maxHealth":1000,"childIndex":1},
        {"rootNode":"HEAD_L","moveCount":1,"maxHealth":100,"parentIndex":0,"childIndex":2},
        {"rootNode":"HEAD_R","moveCount":1,"maxHealth":100,"parentIndex":0,"childIndex":-1}],
      "moves":[{"name":"READY","anim":"READY","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'A'));
    std::array<EnemyView, 2> players;
    players[0].player = 3;
    players[0].position = {0, 0, 10};
    players[1].player = 1;
    players[1].position = {0, 0, 20};
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.target() == 3);
    REQUIRE(actor.child(41));
    REQUIRE(actor.child(42));
    CHECK(actor.child(41)->target() == -1); // equal weakest scores remove the earlier child
    CHECK(actor.child(42)->target() == 3);
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 5;
    actor.hurt(hit, 41);
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.child(41)->target() == 3);
    CHECK(actor.child(42)->target() == 1); // losing player 3 leaves its other eligible candidate
    actor.hurt(hit); // the root's anger raises that player's allowed memberships to four
    actor.update(2, 1.0f / 30, players);
    CHECK(actor.child(41)->target() == 3);
    CHECK(actor.child(42)->target() == 3);
    actor.update(1, 15.25f, players);
    CHECK(actor.child(41)->target() == -1);
    CHECK(actor.child(42)->target() == 3);
}

TEST_CASE("a newly forced head pattern inherits the parent's target after roster pruning",
          "[combatant][multiplayer-targeting]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, test::headedBodyAssets(), bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, {}, 'A'));
    EnemyView player;
    player.player = 3;
    player.position = {0, 0, 40};
    const std::array players{player};
    for (s32 frame = 0; frame < 30 && actor.moveName() != "READYP"; ++frame) {
        actor.update(2, 1.0f / 30, players);
    }
    REQUIRE(actor.moveName() == "READYP");
    REQUIRE(actor.child(41));
    CHECK(actor.child(41)->moveName() == "SPIT");
    CHECK(actor.child(41)->target() == 3);
    CHECK(actor.child(42)->target() == 3);
}

TEST_CASE("boss targets preserve raised hit nodes independently of their floor anchor",
          "[combatant][boss][target-assist]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
        "descriptors":[{"prefix":"BODY","type":4}],
        "types":[{"moveCount":1,"maxHealth":100,"radius":4,
                  "originOffset":[0,28,0],"colCount":1}],
        "moves":[{"name":"READY","anim":"STEP","type":32}],
        "nodes":[{"nodeName":"BODY","position":[0,28,0],"radius":4,
                  "healthScale":1,"damageScale":1}]
    })");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'G');
    REQUIRE(bosses.spawn(41, {0, -25, 0}, 0));
    bosses.wake();
    const auto targets = bosses.targets();
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].node == 0);
    CHECK(targets[0].base == Vec3{0, -1, 0});
    CHECK(targets[0].height == 8);
    CHECK(targets[0].touches({0, 3, 0}, 1));
    CHECK_FALSE(targets[0].touches({0, -21, 0}, 1));
    CHECK(targets[1].node == -1);
    CHECK(targets[1].base == targets[0].base);
    EnemyHit hit;
    hit.damage = 20;
    hit.node = targets[0].node;
    bosses.hurt(hit, targets[0].id);
    CHECK(bosses.view().health == 80);
    hit.damage = 200;
    bosses.hurt(hit);
    CHECK(bosses.targets().empty());
}

/** A DJINN of `root` whose one attack strikes with a ring of ten over its first twenty frames,
 * knocking down, with no effect of its own to carry an area. */
void writeRingDjinn(const std::filesystem::path& root) {
    writeTextFile(root / "MONSTERS/DJINN/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":1},{"name":"ATTACK","frames":30}]}]})");
    writeTextFile(root / "critter/DJINN.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"patternCount":1}],
      "moves":[{"name":"READY","anim":"READY","type":32},
        {"name":"ATTACK","anim":"ATTACK","type":128,"priority":20,"cooldown":100,
         "frameStart":0,"frameEnd":20,"damage0":0}],
      "patterns":[{"moves":[1],"cooldown":100}],
      "damages":[{"type":3,"maxDistance":10,"damage":100,"flags":32}]
    })");
}

TEST_CASE("boss contact windows skip protected attacks without spending their hit",
          "[combatant][attack-invulnerability]") {
    const auto root = familyAssets();
    writeRingDjinn(root);
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'C');
    REQUIRE(bosses.spawn(36, {}, 0, 100));
    bosses.wake();
    std::array<EnemyView, 2> players;
    players[0].player = 0;
    players[0].position = {0, 0, 3};
    players[0].damageable = false;
    players[1].player = 1;
    players[1].position = {0, 0, 4};
    std::vector<CombatBlow> blows;
    for (s32 frame = 0; frame < 20 && blows.empty(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
        blows = bosses.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    CHECK(blows.front().player == 1);
    CHECK(blows.front().flags == EnemyHit::kKnockDown);
    players[0].damageable = true;
    bosses.update(2, 1.0f / 30, players);
    blows = bosses.takeBlows();
    REQUIRE(blows.size() == 1);
    CHECK(blows.front().player == 0);
    bosses.update(2, 1.0f / 30, players);
    CHECK(bosses.takeBlows().empty());
}

TEST_CASE("a ring that carries no area of its own reaches a player by the character's whole "
          "width",
          "[combatant][critter-contact]") {
    // ProcessEffects meets a player col_radius past a ring's reach (0x80095620); the move's own
    // strike, which a ring without an area falls back to, measures the same way.
    const auto root = familyAssets();
    writeRingDjinn(root);
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'C');
    REQUIRE(bosses.spawn(36, {}, 0, 100));
    bosses.wake();
    // Both stand 1.2 past the ring's ten on footprints of three quarters: only the one with a
    // whole width of one and a half is reached.
    std::array<EnemyView, 2> players;
    players[0].player = 0;
    players[0].position = {0, 0, 11.2f};
    players[0].radius = 0.75f;
    players[0].collisionRadius = 1.5f;
    players[1].player = 1;
    players[1].position = {0, 0, -11.2f};
    players[1].radius = 0.75f;
    std::vector<CombatBlow> blows;
    for (s32 frame = 0; frame < 20; ++frame) {
        bosses.update(2, 1.0f / 30, players);
        const auto taken = bosses.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE(blows.size() == 1);
    CHECK(blows.front().player == 0);
    CHECK_FALSE(blows.front().area);
}

TEST_CASE("pattern bosses taunt between attacks without competing with ready priority",
          "[combatant][boss][sound]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
        "descriptors":[{"prefix":"BODY","type":4}],
        "types":[{"moveCount":3,"maxHealth":100,"radius":4,"floorOffset":18.5}],
        "moves":[{"name":"READY","anim":"STEP","type":32,"priority":512},
                 {"name":"TAUNT","anim":"STEP","type":33,"priority":512,
                  "cooldown":10,"sfx":0,"sfxFrame":1},
                 {"name":"LINKED","anim":"STEP","type":33,"flags":4,"sfx":1}],
        "sounds":[{"levelFormat":"TAUNT","offset":[0,100,0]},
                  {"levelFormat":"LINKED_ONLY"}]
    })");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    const Vec3 floor{3, -25, 6};
    REQUIRE(actor.spawn(assets, 0, floor, 0, nullptr, {}, 'G'));
    bool wounded = false;
    SECTION("healthy taunts honor cooldown and keep audio independent of the effect") {}
    SECTION("higher attack rate suppresses fallback taunts") {
        EnemyHit hit;
        hit.damage = 10;
        actor.hurt(hit);
        wounded = true;
    }
    s32 count = 0;
    s32 previous = -1000;
    for (s32 frame = 0; frame < 650; ++frame) {
        actor.update(2, 1.0f / 30, {});
        for (const auto& cue : actor.takeCues()) {
            REQUIRE(cue.sound == "TAUNT");
            CHECK(cue.soundPosition == floor + Vec3{0, 18.5f, 0});
            CHECK(cue.position != cue.soundPosition);
            CHECK(cue.attenuated);
            CHECK(frame - previous >= 300);
            previous = frame;
            ++count;
        }
    }
    CHECK(count == (wounded ? 0 : 3));
}

TEST_CASE("timed combatant effects honor each linked record's no-loop flag",
          "[combatant][garm-death]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
        "descriptors":[{"prefix":"BODY","type":4}],
        "types":[{"moveCount":2,"maxHealth":100,"radius":4}],
        "moves":[{"name":"READY","anim":"STEP","type":32},
                 {"name":"DEATH","anim":"STEP","type":17,"priority":4095,
                  "sfx":0,"sfxFrame":1,"hold":2}],
        "sounds":[{"name":"DEATH_BODY","flags":8388608,"life":300,"link":1},
                  {"name":"DEATH_LOOP","flags":0,"life":300}]
    })");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 10000;
    actor.hurt(hit);
    REQUIRE_FALSE(actor.alive());
    actor.update(2, 1.0f / 30, {});
    const auto cues = actor.takeCues();
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].tree == "DEATH_BODY");
    CHECK_FALSE(cues[0].loop);
    CHECK(cues[0].life == 300);
    CHECK(cues[1].tree == "DEATH_LOOP");
    CHECK(cues[1].loop);
    CHECK(cues[1].life == 300);
    for (s32 frame = 0; frame < 120; ++frame) {
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.takeCues().empty());
    }
    CHECK_FALSE(actor.present());
}

TEST_CASE("combatant death skins blend their own alpha without leaking to a living sibling",
          "[combatant][death-dissolve]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
        "descriptors":[{"prefix":"BODY","type":4}],
        "types":[{"moveCount":2,"maxHealth":100,"radius":4}],
        "moves":[{"name":"READY","anim":"STEP","type":32},
                 {"name":"DEATH","anim":"STEP","type":17,"priority":4095,
                  "sfx":0,"sfxFrame":1,"hold":2}],
        "sounds":[{"name":"SKIN","flags":256,"life":0.05,"rate":0}]
    })");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    REQUIRE(assets.skins.contains("SKIN"));
    Combatant actor;
    Combatant sibling;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    REQUIRE(sibling.spawn(assets, 1, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 10000;
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    REQUIRE(actor.dying());
    device.draws.clear();
    actor.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    CHECK(draw.state.maskedTexture == assets.skins.at("SKIN").front());
    CHECK(draw.blend() == BlendMode::Alpha);
    CHECK_FALSE(draw.state.alphaToCoverage);
    CHECK(draw.state.alphaTest == DrawState::kTranslucentAlphaTest);
    device.draws.clear();
    sibling.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().state.maskedTexture == nullptr);
    CHECK(device.draws.front().state.alphaToCoverage);
}

TEST_CASE("combatants preserve elemental immunity and sub-one damage", "[combatant][damage]") {
    for (const auto& definition : {Golem::definition(), bossDefinition("LICH")}) {
        CAPTURE(definition.name);
        for (u32 element = 1; element <= 5; ++element) {
            CAPTURE(element);
            const u32 shield = element == 5 ? 0x1000 : 1U << (element + 7);
            const u32 flags = element == 5 ? 0x200 : element;
            test::FakeRenderDevice device;
            CombatantAssets assets;
            REQUIRE(assets.load(device, familyAssets(60, shield), definition, 'G'));
            Combatant actor;
            REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
            EnemyHit hit;
            hit.damage = 20;
            hit.flags = flags;
            hit.player = 0;
            actor.hurt(hit);
            CHECK(actor.health() == 100);
            CHECK(actor.takeLosses().empty());
            CHECK(actor.takeCues().empty());
            hit.damage = 0.25f;
            hit.flags = 0;
            actor.hurt(hit);
            CHECK(actor.health() == Approx(99.75f));
        }
    }
}

TEST_CASE("an elemental hit on a great one bursts with its element instead of the own mark",
          "[combatant][damage][damage-types]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":3,"hitSoundClose":0,"hitSoundFar":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60}],
      "sounds":[{"name":"OWNHIT"},{"name":"OWNHITFAR"}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0.5f, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 5;
    hit.player = 0;
    hit.where = Vec3{1, 2, 3};
    const auto only = [&](const EnemyHit& given) {
        actor.hurt(given);
        auto cues = actor.takeCues();
        REQUIRE(cues.size() == 1);
        return cues.front();
    };
    // A blow shows the close mark, a missile the far one (CritterDamage's sfxIndex1 for
    // source 2), and a missile the close one when the type has no far mark.
    hit.close = true;
    CHECK(only(hit).tree == "OWNHIT");
    hit.close = false;
    CHECK(only(hit).tree == "OWNHITFAR");
    // An elemental hit shows the element's burst (fn_800945D0): half the reach, turned the
    // creature's way, where it landed, without the creature's own sound.
    hit.flags = 2;
    const CombatCue burst = only(hit);
    CHECK(burst.tree == "HITCOL");
    CHECK(burst.sound.empty());
    CHECK(burst.scale == Approx(1.5f));
    CHECK(burst.yaw == Approx(0.5f));
    CHECK(burst.position == Vec3{1, 2, 3});
    hit.flags = 1 | 0x10;
    CHECK(only(hit).tree == "FIREHIT");
    // A hit flagged DMG_NOHITFX shows nothing, elemental or not.
    hit.flags = 0x1000000 | 1;
    actor.hurt(hit);
    CHECK(actor.takeCues().empty());
    hit.flags = 0x1000000;
    actor.hurt(hit);
    CHECK(actor.takeCues().empty());
    CHECK(actor.health() < 100);
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":3,"hitSoundClose":0,"hitSoundFar":-1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60}],
      "sounds":[{"name":"OWNHIT"}]})");
    CombatantAssets closeOnly;
    REQUIRE(closeOnly.load(device, root, Gargoyle::definition(), 'G'));
    Combatant other;
    REQUIRE(other.spawn(closeOnly, 0, {}, 0, nullptr, {}, 'G'));
    hit.flags = 0;
    hit.close = false;
    other.hurt(hit);
    auto cues = other.takeCues();
    REQUIRE(cues.size() == 1);
    CHECK(cues.front().tree == "OWNHIT");
}

TEST_CASE("combatant elemental multipliers follow the encounter not the creature family",
          "[combatant][damage]") {
    for (const auto& definition : {Golem::definition(), bossDefinition("LICH")}) {
        test::FakeRenderDevice device;
        CombatantAssets assets;
        REQUIRE(assets.load(device, familyAssets(), definition, 'G'));
        for (const bool bossEncounter : {false, true}) {
            CAPTURE(definition.name, bossEncounter);
            Combatant actor;
            REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr,
                                EnemyScales{.bossEncounter = bossEncounter}, 'G'));
            EnemyHit hit;
            hit.damage = 20;
            hit.flags = 1;
            hit.player = 0;
            actor.hurt(hit);
            CHECK(actor.health() == Approx(bossEncounter ? 75 : 70));
        }
    }
}

TEST_CASE("great-one healing credit precedes underlevel loss but follows armor",
          "[combatant][healing-magic]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,"armor":5}],
      "moves":[{"name":"READY","anim":"STEP","type":32}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    EnemyScales scales;
    scales.playerLevel = 100;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
    EnemyHit hit;
    hit.player = 3;
    hit.level = 80;
    hit.damage = 25;
    CHECK(actor.hurt(hit) == Approx(20)); // armor first, healing second
    CHECK(actor.health() == Approx(88));  // the 40% underlevel loss comes afterwards
    hit.damage = 2;
    CHECK(actor.hurt(hit) == 0); // armor absorbs the whole hit
    CHECK(actor.health() == Approx(88));
    hit.damage = 1000;
    CHECK(actor.hurt(hit) == Approx(88)); // no healing beyond the health left
    CHECK_FALSE(actor.alive());
    CHECK(actor.hurt(hit) == 0);
}

TEST_CASE("boss healing credit includes the party's damage reduction",
          "[combatant][boss][healing-magic]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, familyAssets(), bossDefinition("LICH"), 'G'));
    Combatant actor;
    EnemyScales scales;
    scales.players = 2;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 20;
    CHECK(actor.hurt(hit) == Approx(10));
    CHECK(actor.health() == Approx(90));
}

TEST_CASE("a boss takes harm by the party's size without the under-level loss a great one takes",
          "[game][combatant][critter-damage]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition : {Golem::definition(), bossDefinition("LICH")}) {
        for (const bool rite : {false, true}) {
            CAPTURE(definition.name, rite);
            const bool boss = definition.kind == CombatantKind::Boss;
            CombatantAssets assets;
            REQUIRE(assets.load(device, root, definition, 'G'));
            EnemyScales scales;
            scales.players = 3;
            scales.playerLevel = 100;
            Combatant actor;
            REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
            actor.takeFullHarm(rite);
            EnemyHit hit;
            hit.player = 0;
            hit.level = 80;
            hit.damage = 20;
            // With three playing a boss takes three tenths outside its legend item's rite and
            // pays three times the share; a great one takes the whole hit, then a fiftieth less
            // for each level its hitter is under the place's.
            const f32 credited = boss && !rite ? 6.0f : 20.0f;
            CHECK(actor.hurt(hit) == Approx(credited));
            CHECK(actor.health() == Approx(100.0f - (boss ? credited : 12.0f)));
            const auto losses = actor.takeLosses();
            REQUIRE(losses.size() == 1);
            CHECK(losses[0].experience == Approx(credited / 101.0f * 50.0f * (boss ? 3.0f : 1.0f)));
        }
    }
}

TEST_CASE("a boss counts the party it is set up for in place of the players standing",
          "[game][combatant][critter-damage][party-count]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    const s32 count = GENERATE(0, 2, 3, 4);
    CAPTURE(count);
    // Stood for four, it counts the one player standing once it updates, unless set up for
    // another party.
    EnemyScales scales;
    scales.players = 4;
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
    actor.setPartyCount(count > 0 ? std::optional{count} : std::nullopt);
    const std::array players{EnemyView{.player = 0, .position = Vec3{0, 0, 20}}};
    actor.update(2, 1.0f / 30, players);
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 20;
    const auto counted = static_cast<usize>(count > 0 ? count : 1);
    CHECK(actor.hurt(hit) == Approx(20.0f * Combatant::kBossShares[counted]));
}

TEST_CASE("creature families supply distinct policies without duplicating move execution",
          "[game][combatant]") {
    const auto golem = Golem::definition();
    const auto general = General::definition();
    const auto gargoyle = Gargoyle::definition("gar_lion");
    const auto boss = bossDefinition("lich");
    REQUIRE(golem.name == "GOLEM");
    REQUIRE(general.name == "GENERAL");
    REQUIRE(golem.kind == CombatantKind::Golem);
    REQUIRE(general.kind == CombatantKind::General);
    REQUIRE(gargoyle.kind == CombatantKind::Gargoyle);
    REQUIRE(boss.kind == CombatantKind::Boss);
    REQUIRE(static_cast<s32>(golem.kind) == 3);
    REQUIRE(static_cast<s32>(boss.kind) == 4);
    REQUIRE(static_cast<s32>(gargoyle.kind) == 7);
    REQUIRE(static_cast<s32>(general.kind) == 8);
    REQUIRE(golem.realmCostume);
    REQUIRE(general.realmCostume);
    REQUIRE(golem.knockbackReduction == 5);
    REQUIRE(general.knockbackReduction == 0);
    REQUIRE(golem.dropForm.empty());
    REQUIRE(general.dropForm.empty());
    REQUIRE(gargoyle.name == "GAR_LION");
    REQUIRE(gargoyle.dropForm == "LION");
    REQUIRE_FALSE(gargoyle.realmCostume);
    REQUIRE(gargoyle.selection == CombatantDefinition::Selection::Priority);
    REQUIRE(boss.name == "LICH");
    REQUIRE(boss.selection == CombatantDefinition::Selection::Patterns);
    REQUIRE(boss.boundsToHome);
    REQUIRE_FALSE(golem.boundsToHome);
    REQUIRE_FALSE(general.boundsToHome);
    REQUIRE_FALSE(gargoyle.boundsToHome);
    // The great ones take the best-scored player, are shrunk, shoved and scaled by their
    // hitter's level, and leave a drop; only the golem tramples the swarm.
    for (const CombatantDefinition* family : {&golem, &general, &gargoyle}) {
        CAPTURE(family->name);
        REQUIRE(family->targeting == CombatantDefinition::Targeting::Best);
        REQUIRE_FALSE(family->seesInvisible);
        REQUIRE(family->harmScale == CombatantDefinition::HarmScale::HitterLevel);
        REQUIRE(family->shrinks);
        REQUIRE(family->shoved);
        REQUIRE(family->tramplesSwarm == (family == &golem));
        REQUIRE(family->departure == CombatantDefinition::Departure::Drops);
    }
    REQUIRE(boss.targeting == CombatantDefinition::Targeting::AngerRanked);
    REQUIRE(boss.seesInvisible);
    REQUIRE(boss.harmScale == CombatantDefinition::HarmScale::PartySize);
    REQUIRE_FALSE(boss.shrinks);
    REQUIRE_FALSE(boss.shoved);
    REQUIRE_FALSE(boss.tramplesSwarm);
    REQUIRE(boss.departure == CombatantDefinition::Departure::Fades);
}

TEST_CASE("a great one lies its shadow only when its type says so", "[game][combatant][shadow]") {
    const auto root = familyAssets();
    const auto archive = root / "MONSTERS/GOLEM/LEVELG";
    writeTextFile(archive / "models/flat.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 0 1\nvn 0 -1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1},
      {"index":1,"name":"SHADOW1L1","file":"models/flat.obj","meshTriangles":1}]})");
    test::convertModelFixture(archive);
    for (const u32 typeFlags : {0U, 1U}) {
        CAPTURE(typeFlags);
        writeTextFile(root / "critter/GOLEM.json",
                      R"({"descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
          "types":[{"moveCount":1,"maxHealth":100,"radius":1,"typeFlags":)" +
                          std::to_string(typeFlags) + R"(}],
          "moves":[{"name":"READY","anim":"STEP","type":32}]})");
        test::FakeRenderDevice device;
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
        REQUIRE(assets.data.shadowed() == (typeFlags == 1));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {4, 0, 6}, 0, nullptr, {}, 'G'));
        actor.drawShadow(device, Mat4{1.0f}, Vec3{4, 60, 6}, {});
        if (typeFlags == 0) {
            CHECK(device.draws.empty());
            continue;
        }
        REQUIRE(device.draws.size() == 1);
        CHECK_FALSE(device.draws[0].state.depthWrite);
        CHECK(device.draws[0].vertices[0].position.x == Approx(4));
        CHECK(device.draws[0].vertices[0].position.y ==
              Approx(BlobShadow::kLift + BlobShadow::kPull));
        CHECK(device.draws[0].vertices[0].position.z == Approx(6));
        actor.clear();
        device.draws.clear();
        actor.drawShadow(device, Mat4{1.0f}, Vec3{4, 60, 6}, {});
        CHECK(device.draws.empty());
    }
}

TEST_CASE("one combatant rejects absent assets and owns independent state and events",
          "[game][combatant]") {
    CombatantAssets assets;
    std::array<Combatant, 2> actors;
    REQUIRE_FALSE(actors[0].spawn(assets, 14, {}, 0, nullptr, {}, 'G'));
    REQUIRE_FALSE(actors[0].present());
    test::FakeRenderDevice device;
    REQUIRE(assets.load(device, familyAssets(), Golem::definition(), 'G'));
    REQUIRE(actors[0].spawn(assets, 14, {}, 0, nullptr, {}, 'G'));
    REQUIRE(actors[1].spawn(assets, 42, {20, 0, 0}, 0, nullptr, {}, 'G'));
    REQUIRE(actors[0].archive() == actors[1].archive());
    actors[0].freeze(60);
    REQUIRE_FALSE(actors[1].frozen());
    EnemyHit hit;
    hit.player = 2;
    hit.damage = 12;
    actors[1].hurt(hit);
    REQUIRE(actors[0].health() == 100);
    REQUIRE(actors[1].health() == 88);
    REQUIRE(actors[0].takeLosses().empty());
    const auto losses = actors[1].takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].critter == 42);
    REQUIRE(losses[0].player == 2);
    actors[0].clear();
    REQUIRE_FALSE(actors[0].present());
    REQUIRE(actors[1].present());
    REQUIRE(actors[1].data() != nullptr);
    REQUIRE(actors[1].rootTransform().has_value());
    actors[1].clear();
}

TEST_CASE("combatant hit volumes follow posed nodes and solid flags", "[game][combatant]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":1,"wallRadius":1,"colCount":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32}],
      "nodes":[{"nodeName":"BODY","position":[5,3,0],"radius":2,"flags":8},
               {"nodeName":"BODY","position":[0,8,0],"radius":1,"flags":0},
               {"nodeName":"ABSENT","position":[50,0,0],"radius":9,"flags":8}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 7, {10, 0, 0}, 1.57079633f, nullptr, {}, 'G'));
    const auto targets = actor.bodyTargets();
    REQUIRE(targets.size() == 3); // two real nodes and the root fallback
    CHECK(targets[0].id == 7);
    CHECK(targets[0].base.x == Approx(10));
    CHECK(targets[0].base.z == Approx(-5));
    CHECK(targets[0].base.y == Approx(1));
    CHECK(targets[0].height == 4);
    REQUIRE(actor.bodyTargets(true).size() == 2);
    CHECK(actor.contactDistance({10, 3, -10}, {10, 3, -3}, 0.5f).has_value());
    actor.resize(2);
    CHECK(actor.bodyTargets()[0].radius == 4);
    actor.clear();
    CHECK(actor.bodyTargets().empty());
}

TEST_CASE("breath effect inherits the damage node offset and rotation",
          "[game][combatant][breath]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"FIRE","anim":"STEP","type":128,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":4,"radius":3,"maxDistance":12,"damage":1,"pitch":0.35,
                  "offset":[0,0,-2],"sfxIndex":0}],
      "sounds":[{"name":"FIREFX","offset":[0,0,1],"scale":1}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {10, 0, 0}, 1.57079633f, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 0;
    player.position = {20, 0, 0};
    const std::array players{player};
    std::vector<CombatCue> cues;
    for (s32 i = 0; i < 10 && cues.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        cues = actor.takeCues();
    }
    REQUIRE(cues.size() == 1);
    const auto& cue = cues.front();
    REQUIRE(cue.tree == "FIREFX");
    REQUIRE(cue.node == "BODY");
    REQUIRE(cue.follows);
    REQUIRE(cue.nodeOffset == Vec3{0, 0, -1});
    REQUIRE(cue.pitchYaw.x == Approx(0.35f));
    REQUIRE(cue.position.x == Approx(9));
    REQUIRE(cue.placement.has_value());
    CHECK((*cue.placement)[2].x > 0.9f);
    CHECK((*cue.placement)[2].y < -0.3f);
}

TEST_CASE("a blow's offset turns with its node and reaches by its reach alone",
          "[game][combatant]") {
    const auto root = familyAssets();
    // A claw eight ahead of the body in its own space.
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"CLAW","anim":"STEP","type":128,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":0,"radius":3,"maxDistance":1,"damage":10,"offset":[0,0,8]}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    Combatant actor;
    // Facing -z: the claw is at z -8, where a player standing in front of it is struck.
    REQUIRE(actor.spawn(assets, 0, {0, 0, 0}, 3.14159265f, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, -8};
    player.radius = 0.75f;
    player.height = 5.0f;
    const std::array players{player};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 10 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    CHECK(blows[0].gated); // the player's quarter-second gap holds off the next
    CHECK(blows[0].origin.z == Approx(-8.0f).margin(0.01f));
    CHECK(blows[0].origin.x == Approx(0.0f).margin(0.01f));
}

/** The swinging general's own data: a READY without a node, and a SWING striking with HAND from
 * `frameStart` to frame eight, a blow of 30 that knocks down, reaching three. Its body's origin
 * stands four over its feet. */
formats::CritterFile swingCritter(s32 frameStart) {
    formats::CritterFile source;
    source.descriptors.push_back({"general", "BODY", static_cast<s16>(CombatantKind::General)});
    auto& type = source.types.emplace_back();
    type.radius = 2;
    type.wallRadius = 1;
    type.maxHealth = 100;
    type.moveCount = 2;
    type.originOffset = {0, 4, 0};
    auto& ready = source.moves.emplace_back();
    ready.name = "READY";
    ready.anim = "READY";
    ready.type = MoveDefinition::kReady;
    ready.interrupt = 60;
    auto& swing = source.moves.emplace_back();
    swing.name = "SWING";
    swing.anim = "SWING";
    swing.type = MoveDefinition::kAttackFrom;
    swing.priority = 544;
    swing.colnode = "HAND";
    swing.frameStart = frameStart;
    swing.frameEnd = 8;
    swing.damage0 = 0;
    auto& blow = source.damages.emplace_back();
    blow.type = AttackDefinition::kBlow;
    blow.damage = 30;
    blow.flags = damage::kKnockDown;
    blow.maxDistance = 3;
    return source;
}

/** Writes the swinging general's tree under `root`: a ROOT standing `rootY` over its placement,
 * an ARM five along x from it and a HAND two along x from that. `ready` and `swing` end its
 * READY's and SWING's records. */
void writeSwingTree(const std::filesystem::path& root, s32 rootY, const std::string& ready,
                    const std::string& swing) {
    auto archive = root / "MONSTERS" / General::definition().name;
    if (General::definition().realmCostume) {
        archive /= "LEVELG";
    }
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY","nodes":[
      {"name":"ROOT","object":"BODY","parent":-1,"position":[0,)" +
                                                   std::to_string(rootY) +
                                                   R"(,0]},
      {"name":"ARM","object":"BODY","parent":0,"position":[5,0,0]},
      {"name":"HAND","object":"BODY","parent":1,"position":[2,0,0]}],
      "sequences":[{"name":"READY","frames":30)" + ready +
                                                   R"(},
                   {"name":"SWING","frames":12)" + swing +
                                                   "}]}]}");
}

/** A general whose HAND goes a unit further along x each of SWING's first ten frames, shown
 * `rate` apart (900 over the frames a second); SWING strikes with HAND over frames four to
 * eight. */
void loadSwing(test::FakeRenderDevice& device, CombatantAssets& assets, s32 rate = 30,
               const CritterRules& rules = {}) {
    const auto root = familyAssets();
    writeSwingTree(root, 0, {}, R"(,"frameRate":)" + std::to_string(rate) + R"(,
        "tracks":[{"node":2,"flags":16,"frames":[0,10],"values":[0,10]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    assets.data = CritterData::fromDecoded("GENERAL", swingCritter(4), 0, rules);
}

/** The swinging general with its ROOT three over its placement, rising a tenth a frame through
 * its READY, and a SWING that strikes from its first frame. */
void loadRaisedRoot(test::FakeRenderDevice& device, CombatantAssets& assets) {
    const auto root = familyAssets();
    writeSwingTree(root, 3, R"(,"tracks":[{"node":0,"flags":32,"frames":[0,29],"values":[0,2.9]}])",
                   R"(,"tracks":[{"node":2,"flags":16,"frames":[0,10],"values":[0,10]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    assets.data = CritterData::fromDecoded("GENERAL", swingCritter(0), 0, {});
}

/** The swing's push on a player twelve along x from its feet: the hand's unit along x over a
 * native frame, and the way from the body's origin, a unit high (CritterNodePlayerCollide). */
Vec3 swungPush() {
    return 2.0f * (approximateUnit(Vec3{1, 0, 0}) + approximateUnit(Vec3{12, 1, 0}));
}

/** Its push there over a native frame in which the hand stood still. */
Vec3 stillPush() {
    return 2.0f * approximateUnit(Vec3{12, 1, 0});
}

EnemyView swingTarget() {
    EnemyView player;
    player.player = 0;
    player.position = {12, 0, 0};
    return player;
}

TEST_CASE("a blow pushes along its striking part's swing as well as away from the body, at "
          "thirty and sixty updates a second",
          "[game][combatant][blow-push]") {
    const s32 updates = GENERATE(30, 60);
    CAPTURE(updates);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    loadSwing(device, assets);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    const std::array players{swingTarget()};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < updates && blows.empty(); ++i) {
        actor.update(60 / updates, 1.0f / static_cast<f32>(updates), players);
        blows = actor.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    REQUIRE(actor.moveName() == "SWING");
    CHECK(actor.animation().frame() == 4.0f);
    CHECK(glm::distance(blows[0].direction, swungPush()) < 1e-4f);
    CHECK(blows[0].direction.x == Approx(3.989f).margin(1e-3));
    CHECK(blows[0].direction.y == Approx(0.166f).margin(1e-3));
    CHECK(blows[0].direction.z == 0.0f);
}

TEST_CASE("a rate-60 swing's blow carries the swing of the frame it lands in, on either tick "
          "parity",
          "[game][combatant][blow-push]") {
    const s32 lead = GENERATE(0, 1, 2, 3);
    CAPTURE(lead);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    loadSwing(device, assets, 60);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    constexpr f32 kTick = 1.0f / 60;
    // Standing ready with nobody about, for as many ticks before its swing as the lead.
    for (s32 i = 0; i < lead; ++i) {
        actor.update(1, kTick, {});
    }
    const auto swing = assets.data.moveNamed("SWING");
    REQUIRE(swing);
    Combatant::MovePoint start;
    start.target = 0;
    REQUIRE(actor.setMove(*swing, start));
    const std::array players{swingTarget()};
    std::vector<Vec3> pushes;
    for (s32 i = 0; i < 120 && pushes.size() < 4; ++i) {
        actor.update(1, kTick, players);
        for (const CombatBlow& blow : actor.takeBlows()) {
            pushes.push_back(blow.direction);
        }
    }
    REQUIRE(pushes.size() == 4);
    REQUIRE(actor.animation().frame() == 4.0f);
    // A frame shows for four ticks: the two it first shows on carry the hand's travel into it,
    // the two after none.
    CHECK(glm::distance(pushes[0], swungPush()) < 1e-4f);
    CHECK(glm::distance(pushes[1], swungPush()) < 1e-4f);
    CHECK(glm::distance(pushes[2], stillPush()) < 1e-4f);
    CHECK(glm::distance(pushes[3], stillPush()) < 1e-4f);
}

TEST_CASE("a definition with unitBlowPush keeps the swing's flat way at a unit's length",
          "[game][combatant][blow-push]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    CritterRules rules;
    rules.unitBlowPush = true;
    loadSwing(device, assets, 30, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    // Off the x axis the hand's swing along x and the way out from the body differ.
    EnemyView player = swingTarget();
    player.position.z = 2;
    const std::array players{player};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 30 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    REQUIRE(actor.animation().frame() == 4.0f);
    const Vec3 push = 2.0f * (approximateUnit(Vec3{1, 0, 0}) + approximateUnit(Vec3{12, 1, 2}));
    const Vec3 expected = Vec3{push.x, 0, push.z} / std::hypot(push.x, push.z);
    const Vec3& direction = blows[0].direction;
    CAPTURE(direction.x, direction.y, direction.z);
    CHECK(direction.y == 0.0f);
    CHECK(glm::length(direction) == Approx(1.0f));
    CHECK(glm::distance(direction, expected) < 1e-5f);
    // Not the way straight out from the body.
    CHECK(glm::distance(direction, glm::normalize(Vec3{12, 0, 2})) > 0.05f);
}

TEST_CASE("a blow's offset turns with its node but is not stretched by the node's scale key",
          "[game][combatant][node-scale]") {
    // ARM, five along x, keys four times its length along x through SWING, as the Yeti's hands
    // do through its swipes. GetWorldMat (0x800BB614) composes it without that key, which only
    // the draw applies, so the blow's offset ten back along ARM stays ten back
    // (CritterNodePlayerCollide). Data whose rules keep the scale keys stretch it forty back.
    const bool scaled = GENERATE(false, true);
    CAPTURE(scaled);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    const auto root = familyAssets();
    writeSwingTree(root, 0, {}, R"(,"tracks":[{"node":1,"flags":256,"frames":[0],"values":[4]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    auto source = swingCritter(0);
    source.moves[1].colnode = "ARM";
    source.damages[0].offset = {-10, 0, 0};
    source.damages[0].maxDistance = 1;
    CritterRules rules;
    rules.portRules = scaled ? CritterRules::kScaledNodes : 0;
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    const auto swing = assets.data.moveNamed("SWING");
    REQUIRE(swing);
    Combatant::MovePoint start;
    start.target = 0;
    REQUIRE(actor.setMove(*swing, start));
    // One ten back from ARM, and one forty back, where the stretched arm would put the blow.
    std::array<EnemyView, 2> players{};
    players[0].player = 0;
    players[0].position = {-5, 0, 0};
    players[1].player = 1;
    players[1].position = {-35, 0, 0};
    actor.update(2, 1.0f / 30, players);
    REQUIRE(actor.moveName() == "SWING");
    const auto arm = actor.nodeTransform("ARM");
    REQUIRE(arm);
    REQUIRE(glm::length(Vec3{(*arm)[0]}) == Approx(4)); // drawn four times as long
    const auto blows = actor.takeBlows();
    REQUIRE(blows.size() == 1);
    CHECK(blows[0].player == (scaled ? 1 : 0));
    CHECK(glm::distance(blows[0].origin, Vec3{scaled ? -35.0f : -5.0f, 0, 0}) < 1e-4f);
}

TEST_CASE("a great one's breath, shot, area and hold leave from its node as GetWorldMat places "
          "it, not where its scale keys stretch it",
          "[game][combatant][node-scale]") {
    // ARM, five along x, keys four times its length along x through SWING, which carries the
    // drawn HAND from seven to thirteen along x. GetWorldMat (0x800BB614) composes the nodes
    // without that key, which only the draw applies, so the breath (CritterFirePlayerCollide),
    // the shot's launch node, the area started at HAND and the player it holds all leave HAND
    // at seven. Data whose rules keep the scale keys leave it at thirteen, stretched.
    const bool scaled = GENERATE(false, true);
    const s16 kind = GENERATE(AttackDefinition::kBreath, AttackDefinition::kProjectile,
                              AttackDefinition::kAttachedArea, AttackDefinition::kGrab);
    CAPTURE(scaled, kind);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    const auto root = familyAssets();
    writeSwingTree(root, 0, {}, R"(,"tracks":[{"node":1,"flags":256,"frames":[0],"values":[4]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    auto source = swingCritter(0);
    auto& harm = source.damages[0];
    harm.type = kind;
    harm.damage = 10;
    harm.flags = 0;
    if (kind == AttackDefinition::kBreath) {
        harm.maxDistance = 20;
        harm.radius = 1;
    } else if (kind == AttackDefinition::kAttachedArea) {
        harm.maxDistance = 10;
        harm.minDot = -1;
        harm.offset = {0, 3, 0};
        harm.sfxIndex = 0;
        auto& sound = source.sounds.emplace_back();
        sound.name = "NULLFX";
        sound.life = 0.5f;
        sound.offset = {0, 2, 0};
    } else if (kind == AttackDefinition::kGrab) {
        harm.maxDistance = 1;
        harm.offset = {2, 0, 0};
    } else {
        harm.offset = {0, 3, 0};
    }
    CritterRules rules;
    rules.portRules = scaled ? CritterRules::kScaledNodes : 0;
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    const auto swing = assets.data.moveNamed("SWING");
    REQUIRE(swing);
    Combatant::MovePoint start;
    start.target = 0;
    REQUIRE(actor.setMove(*swing, start));
    const Vec3 hand{scaled ? 13.0f : 7.0f, 0, 0};
    // A player where each meets it from that HAND: ten along the breath, inside the area, or at
    // the grab's reach.
    EnemyView player;
    player.player = 0;
    if (kind == AttackDefinition::kBreath) {
        player.position = hand + Vec3{0, -3, 10};
    } else if (kind == AttackDefinition::kGrab) {
        player.position = hand + Vec3{2, -3, 0};
    } else {
        player.position = hand + Vec3{0, 2, 0};
    }
    actor.update(2, 1.0f / 30, std::array{player});
    REQUIRE(actor.moveName() == "SWING");
    const auto drawn = actor.nodeTransform("HAND");
    REQUIRE(drawn);
    REQUIRE(glm::distance(Vec3{(*drawn)[3]}, Vec3{13, 0, 0}) < 1e-4f); // drawn there either way
    if (kind == AttackDefinition::kBreath) {
        const auto blows = actor.takeBlows();
        REQUIRE(blows.size() == 1);
        REQUIRE(blows[0].breath);
        CHECK(glm::distance(blows[0].origin, hand) < 1e-4f);
    } else if (kind == AttackDefinition::kProjectile) {
        const auto shots = actor.takeShots();
        REQUIRE(shots.size() == 1);
        CHECK(glm::distance(shots[0].origin, hand + Vec3{0, 3, 0}) < 1e-4f);
    } else if (kind == AttackDefinition::kAttachedArea) {
        const auto blows = actor.takeBlows();
        REQUIRE(blows.size() == 1);
        REQUIRE(blows[0].area);
        // The key stretches HAND along x alone, so the offsets above it stand as laid.
        CHECK(glm::distance(blows[0].origin, hand + Vec3{0, 5, 0}) < 1e-4f);
    } else {
        const auto grabs = actor.takeGrabs();
        REQUIRE(grabs.size() == 1);
        REQUIRE(grabs[0].attachment.has_value());
        // The hold's offset, two along HAND's x, stretches with HAND only where the rules keep
        // the key.
        const Mat4& held = *grabs[0].attachment;
        CHECK(glm::length(Vec3{held[0]}) == Approx(scaled ? 4.0f : 1.0f));
        CHECK(glm::distance(Vec3{held[3]}, hand + Vec3{scaled ? 8.0f : 2.0f, 0, 0}) < 1e-4f);
    }
}

TEST_CASE("a blow's swing push follows its node as GetWorldMat places it, not as its scale key "
          "stretches it",
          "[game][combatant][node-scale][blow-push]") {
    // ARM keys its length along x from one to four over SWING's first ten frames, which carries
    // the drawn HAND outward 0.6 a frame. GetWorldMat leaves HAND at seven, so the move node
    // CritterMoveSetup notes (0x8003B1CC) stands still and the blow pushes the player straight
    // away from the body; data whose rules keep the scale keys push along that stretch too.
    const bool scaled = GENERATE(false, true);
    CAPTURE(scaled);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    const auto root = familyAssets();
    writeSwingTree(root, 0, {},
                   R"(,"tracks":[{"node":1,"flags":256,"frames":[0,10],"values":[1,4]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    CritterRules rules;
    rules.portRules = scaled ? CritterRules::kScaledNodes : 0;
    assets.data = CritterData::fromDecoded("GENERAL", swingCritter(4), 0, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 0;
    player.position = {8, 0, 0};
    const std::array players{player};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 30 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    // Away from the body's origin, four over its feet, given a unit's height.
    const Vec3 away{8, 1, 0};
    const Vec3 expected = scaled ? 2.0f * (approximateUnit(Vec3{1, 0, 0}) + approximateUnit(away))
                                 : 2.0f * approximateUnit(away);
    CHECK(glm::distance(blows[0].direction, expected) < 1e-3f);
}

TEST_CASE("a blow's push survives a rewind of its fighter",
          "[game][combatant][blow-push][native-state-digest]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    loadSwing(device, assets);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    NativeStateResources resources;
    REQUIRE(resources.registerCombatantAssets(assets, "test/swing"));
    constexpr f32 kTick = 1.0f / 60;
    const std::array players{swingTarget()};
    // The first blow lands on the seventh tick; saved a native frame before it, the snapshot
    // holds the sample the blow measures from.
    for (s32 i = 0; i < 5; ++i) {
        actor.update(1, kTick, players);
        REQUIRE(actor.takeBlows().empty());
    }
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto push = [&] {
        std::vector<CombatBlow> blows;
        for (s32 i = 0; i < 2; ++i) {
            actor.update(1, kTick, players);
            const auto taken = actor.takeBlows();
            blows.insert(blows.end(), taken.begin(), taken.end());
        }
        REQUIRE(blows.size() == 1);
        return blows[0].direction;
    };
    const Vec3 first = push();
    CHECK(glm::distance(first, swungPush()) < 1e-4f);
    REQUIRE(actor.restoreLocal(saved));
    CHECK(test::liveDigest(actor, resources.resources()) ==
          test::savedDigest(actor, saved, resources.resources()));
    CHECK(push() == first);
}

TEST_CASE("a fighter's hit gap decides when its held blow strikes again",
          "[game][combatant][hit-gap]") {
    // A Dreamcast boss's tenth of a second, and Dark Legacy's quarter.
    const f32 gap = GENERATE(0.1f, 0.25f);
    CAPTURE(gap);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    const auto root = familyAssets();
    // A still hand seven along x, through a SWING of twelve frames shown fifteen a second that
    // strikes in every one of them.
    writeSwingTree(root, 0, {}, R"(,"frameRate":60)");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    auto source = swingCritter(0);
    source.moves[1].frameEnd = 11;
    CritterRules rules;
    rules.playerBreathGap = gap;
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    const auto swing = assets.data.moveNamed("SWING");
    REQUIRE(swing);
    Combatant::MovePoint start;
    start.target = 0;
    REQUIRE(actor.setMove(*swing, start));

    // The player stands by the hand throughout; the level holds it off by its hit gap.
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{8, 0, 0}, 0);
    EnemyView view;
    view.player = 0;
    view.position = players[0].actor.position();
    const std::array views{view};
    s32 tick = 0;
    std::vector<s32> struck;
    test::OpponentScript events;
    events.hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { struck.push_back(tick); };
    test::ScriptedOpponents host{events};
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents level;
    level.open({device, world, weapons, effects, audio, test::scratchDirectory("hit-gap-level"), 1},
               {});
    constexpr f32 kTick = 1.0f / 60;
    f32 held = 0.0f;
    for (; tick < 40 && struck.size() < 2; ++tick) {
        level.update(1, kTick, players, {}, host, host, host); // runs the gap down
        actor.update(1, kTick, views);
        for (const CombatBlow& blow : actor.takeBlows()) {
            LevelOpponents::applyCritterBlow(blow, players, host, host);
        }
        if (struck.size() == 1 && struck[0] == tick) {
            held = players[0].breathGap;
        }
    }
    REQUIRE(actor.moveName() == "SWING");
    REQUIRE(struck.size() == 2);
    CHECK(held == Approx(gap));
    // Not again until the gap has run out, so not at half of it, and again on the update it
    // runs out or the one after, as the clock's rounding falls.
    const s32 gapTicks = static_cast<s32>(std::lround(gap / kTick));
    CAPTURE(struck[0], struck[1]);
    CHECK(struck[1] - struck[0] >= gapTicks);
    CHECK(struck[1] - struck[0] <= gapTicks + 1);
}

TEST_CASE("a move without a node keeps the fighter's root node, which the next move's first "
          "blow measures its jump from",
          "[game][combatant][blow-push]") {
    // CritterMoveSetup notes CritterGetNode's object, and for a move without a node that is
    // c->anim, the tree's root object (critter.c 2011, 3760, 3772-3784), posed and placed.
    test::FakeRenderDevice device;
    CombatantAssets assets;
    loadRaisedRoot(device, assets);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    constexpr f32 kStep = 1.0f / 30;
    for (s32 i = 0; i < 3; ++i) {
        actor.update(2, kStep, {});
    }
    REQUIRE(actor.moveName() == "READY");
    const auto root = actor.nodeTransform("ROOT");
    REQUIRE(root);
    const Vec3 ready{(*root)[3]};
    REQUIRE(ready.y > 3.05f); // its stance has raised it over its rest
    const auto swing = assets.data.moveNamed("SWING");
    REQUIRE(swing);
    Combatant::MovePoint start;
    start.target = 0;
    REQUIRE(actor.setMove(*swing, start));
    EnemyView player;
    player.player = 0;
    player.position = {9, 0, 0};
    actor.update(2, kStep, std::span{&player, 1});
    const auto blows = actor.takeBlows();
    REQUIRE(blows.size() == 1);
    REQUIRE(actor.position() == Vec3{0});
    REQUIRE(actor.yaw() == 0);
    const auto hand = actor.nodeTransform("HAND");
    REQUIRE(hand);
    const Vec3 struck{(*hand)[3]};
    const Vec3 away = approximateUnit(Vec3{9, 1, 0});
    CHECK(glm::distance(blows[0].direction, 2.0f * (approximateUnit(struck - ready) + away)) <
          1e-4f);
    // Measured from the placement instead, the jump would lean up by the root's three units.
    REQUIRE(glm::distance(approximateUnit(struck - ready), approximateUnit(struck)) > 0.1f);
}

TEST_CASE("a sleeping boss takes nothing, and awake takes less the more are in the game",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const s32 players : {1, 3}) {
        CAPTURE(players);
        Bosses boss;
        EnemyScales scales;
        scales.players = players;
        boss.open(device, root, nullptr, scales, 'G');
        REQUIRE(boss.spawn(36, {}, 0));
        const f32 whole = boss.view().health;
        EnemyHit hit;
        hit.player = 0;
        hit.damage = 20;
        boss.hurt(hit); // asleep: nothing, and it sleeps on (CritterDamage, state under two)
        CHECK(boss.view().health == whole);
        CHECK(boss.takeLosses().empty());
        boss.wake();
        boss.hurt(hit);
        // A third of the harm with three playing, each paid three times its share.
        const f32 share = players == 3 ? 0.3f : 1.0f;
        CHECK(boss.view().health == Approx(whole - 20.0f * share));
        const auto losses = boss.takeLosses();
        REQUIRE(losses.size() == 1);
        CHECK(losses[0].experience ==
              Approx(20.0f * share / (1.0f + whole) * 50.0f * static_cast<f32>(players)));
    }
}

TEST_CASE("a boss outside its territory is taken back on a frame it stands still, by the "
          "original's fast length, and left where it dies",
          "[game][combatant][boss-movement]") {
    // DRIDER's home and roam with D5's mark: 24.874 out by the original's measure (CritterTranslate
    // runs its clamp whatever the step, 0x8003ad04-0x8003ae00), 24.466 round.
    const auto root = familyAssets();
    writeTextFile(root / "critter/DJINN.json", R"({
      "descriptors":[{"prefix":"BODY","name":"DJINN","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1,"roamRadius":22,
                "defaultPos":[-30,57,-112]}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
               {"name":"DEATH","anim":"STEP","type":17,"priority":4095,"hold":1}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'C');
    const Vec3 home{-30, 57, -112};
    const Vec3 mark{-52.75f, 61.25f, -121.0f};
    REQUIRE(fixture.spawn("DJINN", mark, 0));
    CHECK(fixture.actor.position() == mark); // spawned where it is put
    fixture.update(2, 1.0f / 30, {});
    const Vec3 offset = mark - home;
    const f32 measured = approximateFlatLength(offset);
    CHECK(measured == Approx(24.874f));
    const Vec3 held = home + offset * (22 / measured);
    CHECK(fixture.actor.position().x == Approx(held.x)); // -50.121
    CHECK(fixture.actor.position().z == Approx(held.z)); // -119.960
    CHECK(held.x == Approx(-50.1214f));
    CHECK(held.z == Approx(-119.9601f));
    // Dying, it is clamped no more (CritterTranslate's state test, 0x8003ad3c-0x8003ad44).
    REQUIRE(fixture.actor.spawn(fixture.assets, 0, mark, 0, nullptr, {}, 'C'));
    EnemyHit lethal;
    lethal.damage = 1000;
    fixture.actor.hurt(lethal);
    REQUIRE(fixture.actor.dying());
    fixture.update(2, 1.0f / 30, {});
    CHECK(fixture.actor.position() == mark);
}

TEST_CASE("a dying boss takes its death's pace in every update until it goes, free of its "
          "territory",
          "[game][combatant][death][boss-death][boss-movement]") {
    const s32 fps = GENERATE(30, 60);
    CAPTURE(fps);
    // CritterTranslate takes the move's pace whatever the state (0x8003a9ec-0x8003aa0c) and holds
    // only a living boss to its territory (0x8003ad3c-0x8003ad44): a DEATH of minus seven, as
    // the Chimera's, backs it away through the death and its hold.
    const auto root = familyAssets();
    writeTextFile(root / "critter/LICH.json", R"({
      "descriptors":[{"prefix":"BODY","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":4,"roamRadius":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
               {"name":"DEATH","anim":"STEP","type":17,"priority":4095,"hold":1,"speed":-7}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    constexpr f32 kYaw = 0.5f;
    const Vec3 home{2, 0, 5};
    REQUIRE(actor.spawn(assets, 0, home, kYaw, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 10000;
    hit.direction = Vec3{0};
    actor.hurt(hit);
    REQUIRE(actor.dying());
    const s32 ticks = std::max(1, Combatant::kTicksPerSecond / fps);
    const f32 seconds = 1.0f / static_cast<f32>(fps);
    const Vec3 backward{-std::sin(kYaw), 0, -std::cos(kYaw)};
    Vec3 last = actor.position();
    s32 steps = 0;
    for (s32 update = 0; update < 10 * fps && actor.present(); ++update) {
        actor.update(ticks, seconds, {});
        if (actor.present()) {
            const Vec3 moved = actor.position() - last;
            CHECK(moved.x == Approx(7 * seconds * backward.x));
            CHECK(moved.z == Approx(7 * seconds * backward.z));
            last = actor.position();
            ++steps;
        }
    }
    REQUIRE_FALSE(actor.present());
    // STEP's tenth of a second and its hold of one carry it some 7.7 back, past its three.
    const f32 back = glm::length(Vec2{last.x - home.x, last.z - home.z});
    CHECK(back == Approx(7 * seconds * static_cast<f32>(steps)));
    CHECK(back > 7);
}

TEST_CASE("boss damage and experience follow standing slots after players leave combat",
          "[combatant][multiplayer-targeting][combatant-active-count]") {
    const auto root = test::headedBodyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("CHIMERA"), 'A'));
    Combatant actor;
    EnemyScales scales;
    scales.players = 4;
    REQUIRE(actor.spawn(assets, 40, {}, 0, nullptr, scales, 'A'));
    std::array<EnemyView, 4> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].player = static_cast<s32>(players.size() - i - 1);
        players[i].position = {0, 0, 40};
    }
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 10;
    const auto checkHarm = [&](s32 id, f32 expectedDamage, s32 active) {
        const Combatant* victim = id == actor.id() ? &actor : actor.child(id);
        REQUIRE(victim != nullptr);
        const f32 before = victim->health();
        actor.hurt(hit, id);
        CHECK(victim->health() == Approx(before - expectedDamage));
        const auto losses = actor.takeLosses();
        REQUIRE(losses.size() == 1);
        CHECK(losses.front().experience ==
              Approx(expectedDamage / (1.0f + victim->maxHealth()) * victim->data()->experience() *
                     static_cast<f32>(active)));
    };
    actor.update(2, 1.0f / 30, players);
    checkHarm(40, 2, 4);
    checkHarm(41, 2, 4);
    // Invisible, invulnerable and captured still means standing; only life hides a slot.
    players[0].invisible = true;
    players[0].damageable = false;
    players[0].captured = true;
    for (usize i = 1; i < players.size(); ++i) {
        players[i].hidden = true;
    }
    actor.update(2, 1.0f / 30, players);
    checkHarm(40, 10, 1);
    checkHarm(41, 10, 1);
    // A sparse roster has the same meaning as retained hidden runtimes.
    actor.update(2, 1.0f / 30, std::span<const EnemyView>{players}.first(1));
    checkHarm(42, 10, 1);
    actor.update(2, 1.0f / 30, {});
    checkHarm(40, 10, 0); // zero's damage multiplier is one, its experience multiplier zero
}

TEST_CASE("a fallen great one leaves its drop as its death ends; a fallen boss fades without one",
          "[game][combatant][death]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition : {Golem::definition(), General::definition(),
                                   Gargoyle::definition(), bossDefinition("LICH")}) {
        CAPTURE(definition.name);
        const bool boss = definition.kind == CombatantKind::Boss;
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 7, {3, 0, 4}, 0, nullptr, {}, 'G'));
        EnemyHit hit;
        hit.player = 0;
        hit.damage = 1000;
        hit.direction = Vec3{0}; // no shove, so the body falls where it stood
        actor.hurt(hit);
        REQUIRE(actor.dying());
        std::vector<CombatLoss> drops;
        for (s32 frame = 0; frame < 90 && actor.present(); ++frame) {
            actor.update(2, 1.0f / 30, {});
            for (const CombatLoss& loss : actor.takeLosses()) {
                if (loss.drop) {
                    drops.push_back(loss);
                }
            }
        }
        CHECK_FALSE(actor.present());
        REQUIRE(drops.size() == (boss ? 0 : 1));
        if (!boss) {
            CHECK(drops[0].critter == 7);
            CHECK(drops[0].kind == definition.kind);
            CHECK(drops[0].form == definition.dropForm);
            CHECK(drops[0].position == Vec3{3, 0, 4});
        }
    }
}

/** How many updates of `seconds` a hold of `hold` lasts once a move's animation is done, summed
 * in f32 from the last update the animation ran, as a fighter counts it. */
s32 heldUpdates(f32 hold, f32 seconds) {
    f32 held = 0;
    s32 updates = 0;
    while (held < hold) {
        held += seconds;
        ++updates;
    }
    return updates;
}

/** A boss of the family archive's LICH whose moves are `moves`, its STEP three frames long. */
std::filesystem::path heldMoveBoss(std::string_view moves) {
    const auto root = familyAssets();
    const std::string table = R"({"descriptors":[{"prefix":"BODY","type":4}],
        "types":[{"moveCount":3,"maxHealth":100,"radius":4}],"moves":[)";
    writeTextFile(root / "critter/LICH.json", table + std::string(moves) + "]}");
    return root;
}

TEST_CASE("a boss goes the update its death's hold runs out, counted from the last update its "
          "death ran, fading over the last half second of it",
          "[game][combatant][death][boss-death]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    const auto root = heldMoveBoss(R"({"name":"READY","anim":"STEP","type":32},
        {"name":"DEATH","anim":"STEP","type":17,"priority":4095,"hold":2})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    CHECK_FALSE(actor.deathPlayed());
    EnemyHit hit;
    hit.damage = 10000;
    actor.hurt(hit);
    REQUIRE(actor.dying());
    CHECK_FALSE(actor.deathPlayed());
    const s32 ticks = std::max(1, Combatant::kTicksPerSecond / fps);
    const f32 seconds = 1.0f / static_cast<f32>(fps);
    s32 lastRan = -1;
    f32 held = 0;
    s32 update = 0;
    while (update < 10 * fps) {
        ++update;
        actor.update(ticks, seconds, {});
        if (!actor.present()) {
            break;
        }
        REQUIRE(actor.moveName() == "DEATH");
        if (!actor.animation().finished()) {
            lastRan = update;
            CHECK_FALSE(actor.deathPlayed());
            continue;
        }
        // CritterBossAI sets gBossDead from the first frame the death is done and its hold runs.
        CHECK(actor.deathPlayed());
        held += seconds;
        const auto visuals = actor.visuals();
        REQUIRE_FALSE(visuals.empty());
        CHECK(visuals.front().alpha == Approx(std::clamp((2.0f - held) / 0.5f, 0.0f, 1.0f)));
    }
    REQUIRE(lastRan > 0);
    REQUIRE_FALSE(actor.present());
    // Sixty updates of 1/30 s come to just under two seconds in f32, so it goes on the 61st
    // after its death last ran, one after the first finished one's sixtieth; 121 at 60 a second
    // and 241 at 120.
    CHECK(update - lastRan == heldUpdates(2, seconds));
    CHECK(update - lastRan == (fps == 30 ? 61 : (fps == 60 ? 121 : 241)));
}

TEST_CASE("the update a headed boss goes in sets nothing off after it goes",
          "[game][combatant][death][boss-death]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, test::headedBodyAssets(), bossDefinition("CHIMERA"), 'A'));
    Combatant body;
    REQUIRE(body.spawn(assets, 0, {}, 0, nullptr, {}, 'A'));
    REQUIRE(body.childCount() == 2);
    EnemyHit hit;
    hit.damage = 100000;
    body.hurt(hit);
    REQUIRE(body.dying());
    s32 update = 0;
    while (body.present() && update < 600) {
        body.takeCues();
        body.update(2, 1.0f / 30, {});
        ++update;
    }
    REQUIRE_FALSE(body.present());
    // CritterBossAI returns once CritterDelInst has removed the boss (0x8003a0c4-0x8003a0cc),
    // before the move's frames, its step and its heads' steps (0x8003a1f4-0x8003a2b4): nothing
    // the body or its heads would do later in the update is heard of.
    CHECK(body.takeCues().empty());
    CHECK(body.takeBlows().empty());
    CHECK(body.takeGrabs().empty());
}

TEST_CASE("a boss's held move gives way to the next the update its hold runs out, counted from "
          "the last update it ran, a linked entrance's as an attack's",
          "[game][combatant][boss-attacks][move-hold]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    const s32 ticks = std::max(1, Combatant::kTicksPerSecond / fps);
    const f32 seconds = 1.0f / static_cast<f32>(fps);
    test::FakeRenderDevice device;
    // Updates `actor` until it leaves move `name`: how many updates after the last one `name`'s
    // animation ran that took, or -1 if it never left.
    const auto leaves = [&](Combatant& actor, std::string_view name,
                            std::span<const EnemyView> players) {
        s32 lastRan = 0;
        for (s32 update = 1; update < 20 * fps; ++update) {
            actor.update(ticks, seconds, players);
            if (actor.moveName() != name) {
                return update - lastRan;
            }
            if (!actor.animation().finished()) {
                lastRan = update;
            }
        }
        return -1;
    };
    SECTION("PBOSS's SPOUT") {
        const auto root = heldMoveBoss(R"({"name":"READY","anim":"STEP","type":32},
            {"name":"SPOUT","anim":"STEP","type":135,"priority":20,"hold":1.5,"cooldown":1000,
             "target":{"maxDistance":50}},
            {"name":"DEATH","anim":"STEP","type":17,"priority":4095})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        const std::array party{EnemyView{.player = 0, .position = Vec3{0, 0, 10}}};
        for (s32 update = 0; update < fps && actor.moveName() != "SPOUT"; ++update) {
            actor.update(ticks, seconds, party);
        }
        REQUIRE(actor.moveName() == "SPOUT");
        CHECK(leaves(actor, "SPOUT", party) == heldUpdates(1.5f, seconds));
    }
    SECTION("the Wraith's START and START2") {
        const auto root = heldMoveBoss(R"({"name":"READY","anim":"STEP","type":32},
            {"name":"START","anim":"STEP","type":16,"priority":4095,"hold":2,"link":2},
            {"name":"START2","anim":"STEP","type":16,"priority":4095,"hold":3.8})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, bossDefinition("LICH"), 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        REQUIRE(actor.moveName() == "START");
        CHECK(leaves(actor, "START", {}) == heldUpdates(2, seconds));
        REQUIRE(actor.moveName() == "START2");
        CHECK(leaves(actor, "START2", {}) == heldUpdates(3.8f, seconds));
        CHECK(actor.moveName() == "READY");
    }
}

TEST_CASE("asset loading rejects a descriptor from the wrong combatant family",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    for (const auto& definition : {Golem::definition(), General::definition(),
                                   Gargoyle::definition(), bossDefinition("DJINN")}) {
        REQUIRE(assets.load(device, root, definition, 'G'));
        REQUIRE(assets.data.kind() == definition.kind);
        auto wrong = definition;
        wrong.kind =
            definition.kind == CombatantKind::Boss ? CombatantKind::Golem : CombatantKind::Boss;
        REQUIRE_FALSE(assets.load(device, root, wrong, 'G'));
        REQUIRE(assets.tree == nullptr);
        REQUIRE_FALSE(assets.archive.loaded());
        Combatant actor;
        REQUIRE_FALSE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        REQUIRE_FALSE(actor.present());
    }
    auto unknown = Golem::definition();
    unknown.kind = CombatantKind::Unknown;
    REQUIRE_FALSE(assets.load(device, root, unknown, 'G'));
    Critters population;
    population.open(device, root, nullptr, {}, 'G');
    REQUIRE_FALSE(population.spawn(CombatantKind::Unknown, {}, 0).has_value());
    REQUIRE(population.kindOf(-1) == CombatantKind::Unknown);
    REQUIRE(population.kindOf(0) == CombatantKind::Unknown);
}

constexpr u32 kKnockOver = 0x100; ///< a hit that knocks a great one over

TEST_CASE("ordinary critter movement sweeps player collision volumes in three dimensions",
          "[combatant][critter-body-contact][critter-contact]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":2,"wallRadius":1,
                "floorOffset":2,"originOffset":[0,4,0]}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":0}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 3;
    player.position = {1, 0, 0};
    player.collisionHeight = 6.0f;
    f32 seconds = 1.0f / 30;
    bool blocked = true;
    std::optional<f32> push;
    SECTION("same-floor inward movement stops") {}
    SECTION("a player on another floor does not obstruct it") {
        player.position.y = 20;
        blocked = false;
    }
    SECTION("a step leaving an existing overlap is allowed") {
        player.position.x = -1;
        blocked = false;
    }
    SECTION("a long step cannot pass through a player") {
        player.position.x = 5;
        seconds = 1;
    }
    SECTION("wall radius is horizontal while radius is vertical") {
        player.position.x = 2.5f;
        blocked = false;
    }
    SECTION("the player's whole width meets the wall radius") {
        // CritterCollidePlayers widens the body by col_radius, the class's whole width.
        player.position.x = 2.3f;
        player.radius = 0.75f;
        player.collisionRadius = 1.5f;
    }
    SECTION("the player's whole width deepens the push") {
        // It pushes by twice the wall radius and col_radius past the separation, held to one
        // to three: 1 + 1.5 - (1.2 - 1/3) once the step's third of a unit is taken.
        player.position.x = 1.2f;
        player.radius = 0.75f;
        player.collisionRadius = 1.5f;
        push = 2 * (1 + 1.5f - (1.2f - 10.0f / 30));
    }
    SECTION("native collision centre overrides visual height") {
        player.position.y = -12;
        player.collisionHeight = 18.0f;
    }
    SECTION("invisible players retain physical contact") {
        player.invisible = true;
    }
    SECTION("hidden players do not obstruct it") {
        player.hidden = true;
        blocked = false;
    }
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = kKnockOver;
    hit.direction = {1, 0, 0};
    actor.hurt(hit);
    actor.update(2, seconds, std::span{&player, 1});
    CHECK(actor.position().x == Approx(blocked ? 0.0f : 10.0f * seconds));
    const auto pushes = actor.takePushes();
    REQUIRE(pushes.size() == (blocked ? 1 : 0));
    if (blocked) {
        CHECK(pushes.front().player == 3);
        CHECK(glm::length(pushes.front().velocity) >= 2.0f);
        CHECK(glm::length(pushes.front().velocity) <= 6.0f);
        if (push) {
            CHECK(glm::length(pushes.front().velocity) == Approx(*push));
        }
    }
    CHECK(actor.takePushes().empty());
}

TEST_CASE("node-based critters use solid animated parts for player movement contact",
          "[combatant][critter-body-contact][critter-contact]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":2,"wallRadius":1,
                "typeFlags":256,"colCount":1}],
      "nodes":[{"nodeName":"BODY","position":[5,6,0],"radius":1,"flags":8}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":0}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    REQUIRE(assets.data.parts().size() == 1);
    REQUIRE(assets.data.typeFlags() == 256);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyView player;
    player.player = 1;
    player.position = {6, 0, 0};
    player.collisionHeight = 6.0f;
    bool blocked = true;
    SECTION("the authored solid node stops the movement") {}
    SECTION("no artificial body fallback fills the gap behind that node") {
        player.position.x = 1;
        blocked = false;
    }
    SECTION("a player above that node is not in contact") {
        player.position.y = 20;
        blocked = false;
    }
    SECTION("the player's whole width holds the walk off") {
        // CritterMoveNodeColSub widens a solid node by col_radius, the class's whole width.
        player.position.x = 7.4f;
        player.radius = 0.75f;
        player.collisionRadius = 1.5f;
    }
    SECTION("a footprint without a contact width keeps the narrower contact") {
        player.position.x = 7.4f;
        player.radius = 0.75f;
        blocked = false;
    }
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = kKnockOver;
    hit.direction = {1, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, std::span{&player, 1});
    CHECK(actor.position().x == Approx(blocked ? 0.0f : 10.0f / 30));
    const auto pushes = actor.takePushes();
    REQUIRE(pushes.size() == (blocked ? 1 : 0));
    if (blocked) {
        CHECK(pushes.front().velocity == Vec3{2, 0, 0});
    }
}

/** The turned-part general's own data: a stance, and one solid part two along x from ARM. */
formats::CritterFile turnedPartCritter() {
    formats::CritterFile source;
    source.descriptors.push_back({"general", "BODY", static_cast<s16>(CombatantKind::General)});
    auto& type = source.types.emplace_back();
    type.typeFlags = CritterData::kSolidParts;
    type.radius = 2;
    type.wallRadius = 1;
    type.maxHealth = 100;
    type.moveCount = 1;
    type.colCount = 1;
    auto& ready = source.moves.emplace_back();
    ready.name = "READY";
    ready.anim = "STEP";
    ready.type = MoveDefinition::kReady;
    auto& arm = source.nodes.emplace_back();
    arm.nodeName = "ARM";
    arm.flags = static_cast<s16>(CritterPart::kSolid);
    arm.position = {2, 0, 0};
    arm.radius = 1;
    return source;
}

/** How the turned-part general's ROOT stands through its stance. */
enum class TurnedRoot : u8 {
    Still,
    Turned,    ///< a quarter about y
    Scaled,    ///< keyed twice its length along x
    PlaceOnly, ///< turned, and flagged to hand its children its place alone
};

/** A general whose ARM stands five along x from its ROOT and is turned a quarter about y
 * through its stance, its ROOT standing as `stance` says, of `source` under `rules`. */
void loadTurnedPart(test::FakeRenderDevice& device, CombatantAssets& assets, TurnedRoot stance,
                    const CritterRules& rules = {},
                    const formats::CritterFile& source = turnedPartCritter()) {
    const auto root = familyAssets();
    auto archive = root / "MONSTERS" / General::definition().name;
    if (General::definition().realmCostume) {
        archive /= "LEVELG";
    }
    const std::string quarterTurn = R"("flags":2,"frames":[0],"values":[1.5707964])";
    std::string rootTrack;
    if (stance == TurnedRoot::Turned || stance == TurnedRoot::PlaceOnly) {
        rootTrack = R"({"node":0,)" + quarterTurn + "},";
    } else if (stance == TurnedRoot::Scaled) {
        rootTrack = R"({"node":0,"flags":256,"frames":[0],"values":[2]},)";
    }
    const std::string rootFlags =
        stance == TurnedRoot::PlaceOnly
            ? R"(,"objectFlags":)" + std::to_string(TreePose::kTranslationOnlyParent)
            : "";
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY","nodes":[
      {"name":"ROOT","object":"BODY","parent":-1,"position":[0,0,0])" +
                                                   rootFlags + R"(},
      {"name":"ARM","object":"BODY","parent":0,"position":[5,0,0]}],
      "sequences":[{"name":"STEP","frames":3,"tracks":[)" +
                                                   rootTrack + R"({"node":1,)" + quarterTurn +
                                                   "}]}]}]}");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, rules);
}

TEST_CASE("a solid part's offset sits in its node's parent frame, as the original's GetWorldMat "
          "places it",
          "[combatant][critter-contact]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    Combatant actor;
    // Level with the part and 2.2 along x from where GetWorldMat (0x800BB614) puts it: within
    // the general's knocked step there, out of reach of the part turned with ARM to (5, 0, -2).
    EnemyView player;
    player.player = 1;
    player.position = {9.2f, -3, 0};
    const auto knockedStep = [&] {
        EnemyHit hit;
        hit.damage = 1;
        hit.flags = kKnockOver;
        hit.direction = {1, 0, 0};
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, std::span{&player, 1});
    };
    const auto solidCentre = [&] {
        const auto targets = actor.bodyTargets(true);
        REQUIRE(targets.size() == 2); // the part, then the body
        REQUIRE(targets[0].node == 0);
        return targets[0].base + Vec3{0, targets[0].radius, 0};
    };
    SECTION("the node's own turn leaves the offset where its parent holds it") {
        loadTurnedPart(device, assets, TurnedRoot::Still);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        const auto node = actor.nodeTransform("ARM");
        REQUIRE(node.has_value());
        REQUIRE(glm::distance(Vec3{*node * Vec4{2, 0, 0, 1}}, Vec3{5, 0, -2}) < 1e-4f);
        CHECK(glm::distance(solidCentre(), Vec3{7, 0, 0}) < 1e-4f);
        knockedStep();
        CHECK(actor.position().x == 0);
        const auto pushes = actor.takePushes();
        REQUIRE(pushes.size() == 1);
        CHECK(pushes.front().player == 1);
        CHECK(glm::distance(pushes.front().velocity, Vec3{2, 0, 0}) < 1e-4f);
    }
    SECTION("the parent's turn carries the offset with it") {
        loadTurnedPart(device, assets, TurnedRoot::Turned);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        CHECK(glm::distance(solidCentre(), Vec3{0, 0, -7}) < 1e-4f);
    }
    SECTION("the parent's scale key neither carries the node out nor stretches the offset") {
        // Only the draw applies a scale key; GetWorldMat composes the nodes without it.
        loadTurnedPart(device, assets, TurnedRoot::Scaled);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        const auto node = actor.nodeTransform("ARM");
        REQUIRE(node.has_value());
        REQUIRE(glm::distance(Vec3{(*node)[3]}, Vec3{10, 0, 0}) < 1e-4f); // drawn there
        CHECK(glm::distance(solidCentre(), Vec3{7, 0, 0}) < 1e-4f);
    }
    SECTION("data whose rules keep the scale keys carry the node out and stretch the offset") {
        CritterRules rules;
        rules.portRules = CritterRules::kScaledNodes;
        loadTurnedPart(device, assets, TurnedRoot::Scaled, rules);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        CHECK(glm::distance(solidCentre(), Vec3{14, 0, 0}) < 1e-4f);
    }
    SECTION("a parent that hands its place alone leaves the offset in the frame above it") {
        // GetWorldMat adds such a parent's translation without its turn (0x800BB690-0x800BB6E0).
        loadTurnedPart(device, assets, TurnedRoot::PlaceOnly);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        CHECK(glm::distance(solidCentre(), Vec3{7, 0, 0}) < 1e-4f);
    }
    SECTION("data whose rules keep the node's frame turns the offset with the node") {
        CritterRules rules;
        rules.nodeFramePartOffsets = true;
        loadTurnedPart(device, assets, TurnedRoot::Still, rules);
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        CHECK(glm::distance(solidCentre(), Vec3{5, 0, -2}) < 1e-4f);
        knockedStep();
        CHECK(actor.position().x == Approx(10.0f / 30));
        CHECK(actor.takePushes().empty());
    }
}

TEST_CASE("a great one's step meets another's part, the swarm and the level's items where "
          "GetWorldMat places the parts",
          "[combatant][critter-contact]") {
    // Each stands within reach of the knocked step's third of a unit along x where GetWorldMat
    // (0x800BB614) puts the part, and out of reach of the part turned with ARM.
    test::FakeRenderDevice device;
    CombatantAssets assets;
    auto source = turnedPartCritter();
    source.types.front().typeFlags |= CritterData::kByParts; // another's parts stop it
    loadTurnedPart(device, assets, TurnedRoot::Still, {}, source);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    Combatant peer;
    std::span<const Combatant> peers;
    std::vector<EnemyBody> swarm;
    std::vector<CombatantObstacle> items;
    SECTION("another great one's part") {
        // Turned half about at (9.2, 0, 0), its part stands at (2.2, 0, 0), 1.87 from the step's
        // end, within the wall radius and the part's; turned with ARM it would stand at (4.2, 0,
        // 2).
        REQUIRE(peer.spawn(assets, 1, {9.2f, 0, 0}, kPi, nullptr, {}, 'G'));
        peers = std::span{&peer, 1};
    }
    SECTION("one of the swarm") {
        // The part goes from (7, 0, 0) to 2.2 short of the body, within the two radii and the
        // part's 2.5; turned it would go to (5.33, 0, -2), 4.65 off.
        swarm.push_back({0, {7 + 10.0f / 30 + 2.2f, 0, 0}, 0.5f, 1});
        actor.setSwarm(swarm);
    }
    SECTION("an item") {
        // A box from 8.2 to 9.2 along x: the part's unit reaches it from (7.33, 0, 0), not from
        // where it starts, nor turned from (5.33, 0, -2).
        items.push_back(
            {Obstacle{.centre = {8.7f, 0, 0}, .halfAcross = 0.5f, .halfAlong = 0.5f, .height = 4}});
        actor.setObstacles(items);
    }
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = kKnockOver;
    hit.direction = {1, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {}, peers);
    CHECK(actor.position().x == 0);
}

TEST_CASE("a broken part's shot leaves where GetWorldMat places the part",
          "[combatant][critter-contact][shrink]") {
    // A part broken by an overrun lets its damage record go from the part's place, its offset
    // in ARM's parent's frame (CritterUpdateColnodes), not turned with ARM to (5, 0, -2). Shrunk,
    // the part stands whole and the record's own offset shrinks with the drawn body, as
    // CritterDoTexmodNode shrinks it (0x8003D10C-0x8003D154), and so does the shot's reach
    // (0x8003D410-0x8003D424).
    const f32 shrink = GENERATE(1.0f, 0.667f);
    CAPTURE(shrink);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    auto source = turnedPartCritter();
    auto& part = source.nodes.front();
    part.flags = static_cast<s16>(CritterPart::kSolid | CritterPart::kBreakable);
    part.sfxIndex = 0;
    part.healthScale = 0.1f;
    auto& shot = source.damages.emplace_back();
    shot.type = AttackDefinition::kProjectile;
    shot.damage = 10;
    shot.offset = {0, 3, 0};
    loadTurnedPart(device, assets, TurnedRoot::Still, {}, source);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    actor.setShrink(shrink);
    EnemyHit hit;
    hit.damage = 50;
    hit.node = 0;
    actor.hurt(hit);
    const auto shots = actor.takeShots();
    REQUIRE(shots.size() == 1);
    CHECK(glm::distance(shots.front().origin, Vec3{7, 3 * shrink, 0}) < 1e-4f);
    CHECK(shots.front().scale == shrink);
}

/** Where the original's GetWorldMat (0x800BB614) puts `offset` at node `node` of `pose`, in the
 * model's space: the node's local matrix without its scale key, the offset added to its
 * translation, then each parent's matrix, also without its scale key, composed over it, one
 * flagged to hand its place alone adding its translation. */
Vec3 worldMatPlace(const TreeInfo& tree, const TreePose& pose, usize node, const Vec3& offset) {
    const auto unscaled = [&](usize at) {
        NodePose local = pose.poses()[at];
        local.scale = Vec3{1.0f};
        return TreePose::localMatrix(local, tree.nodes[at].position);
    };
    Mat4 placed = unscaled(node);
    placed[3] += Vec4{offset, 0};
    for (s32 parent = tree.nodes[node].parent; parent >= 0;
         parent = tree.nodes[static_cast<usize>(parent)].parent) {
        const Mat4 above = unscaled(static_cast<usize>(parent));
        if ((tree.nodes[static_cast<usize>(parent)].objectFlags &
             TreePose::kTranslationOnlyParent) != 0) {
            placed[3] += Vec4{Vec3{above[3]}, 0};
        } else {
            placed = above * placed;
        }
    }
    return Vec3{placed[3]};
}

TEST_CASE("great ones' parts stand where the original's GetWorldMat places them through every "
          "move, the ice golem's far from where their own turns swing them",
          "[combatant][critter-contact][assets]") {
    struct Form {
        std::string_view critter;   ///< its CRITTER wad
        std::string_view animation; ///< its costume's animations
        CombatantDefinition definition;
        char realm = 'G';
        bool swungFar = false; ///< whether its own turns swing a part units away
    };
    const std::array forms{
        Form{"CRITTER/GOLEMI.WAD", "MONSTERS/GOLEM/LEVELI/ANIM.PS2", Golem::definition('I'), 'I',
             true},
        Form{"CRITTER/GOLEM.WAD", "MONSTERS/GOLEM/LEVELG/ANIM.PS2", Golem::definition('G'), 'G'},
        Form{"CRITTER/GOLEM.WAD", "MONSTERS/GOLEM/LEVELA/ANIM.PS2", Golem::definition('A'), 'A'},
        Form{"CRITTER/GOLEMF.WAD", "MONSTERS/GOLEM/LEVELF/ANIM.PS2", Golem::definition('F'), 'F'},
        Form{"CRITTER/GAR_EAGL.WAD", "MONSTERS/GAR_EAGL/ANIM.PS2", Gargoyle::definition("EAGL")},
        Form{"CRITTER/GAR_LION.WAD", "MONSTERS/GAR_LION/ANIM.PS2", Gargoyle::definition("LION")},
        Form{"CRITTER/GAR_SERP.WAD", "MONSTERS/GAR_SERP/ANIM.PS2", Gargoyle::definition("SERP")}};
    const auto& form = forms[static_cast<usize>(GENERATE(0, 1, 2, 3, 4, 5, 6))];
    const auto root = test::assetOrSkip(form.critter).parent_path().parent_path();
    test::assetOrSkip(form.animation);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, form.definition, form.realm));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, form.realm));
    const TreeInfo& tree = *assets.tree;
    f32 misplaced = 0; ///< the furthest a part's volume stands from GetWorldMat's place
    f32 rise = 0;  ///< the furthest that place lies above or below the part turned with its node
    f32 shift = 0; ///< and along the ground
    usize poses = 0;
    for (usize move = 0; move < assets.data.moves().size(); ++move) {
        const auto sequence = tree.findSequence(assets.data.moves()[move].anim);
        if (!sequence) {
            continue;
        }
        for (s32 frame = 0; frame < std::max(1, tree.sequences[*sequence].frames); ++frame) {
            Combatant::MovePoint point;
            point.frame = static_cast<f32>(frame);
            REQUIRE(actor.setMove(move, point));
            const auto model = actor.nodeTransform({});
            REQUIRE(model.has_value());
            const TreePose pose = actor.presentedPose(-1);
            for (const auto& volume : actor.bodyTargets()) {
                if (volume.node < 0) {
                    continue;
                }
                const auto& part = assets.data.parts()[static_cast<usize>(volume.node)];
                const auto node = tree.findNode(part.node, kCombatantNodeNameLength);
                REQUIRE(node.has_value());
                const Vec3 placed{*model *
                                  Vec4{worldMatPlace(tree, pose, *node, part.position), 1}};
                const Vec3 centre = volume.base + Vec3{0, 0.5f * volume.height, 0};
                const Vec3 turned{*actor.nodeTransform(part.node) * Vec4{part.position, 1}};
                misplaced = std::max(misplaced, glm::distance(centre, placed));
                rise = std::max(rise, std::abs(placed.y - turned.y));
                shift =
                    std::max(shift, glm::length(Vec2{placed.x - turned.x, placed.z - turned.z}));
                ++poses;
            }
        }
    }
    CAPTURE(form.critter, form.animation, poses, misplaced, rise, shift);
    REQUIRE(poses > 0);
    CHECK(misplaced < 1e-3f);
    if (form.swungFar) {
        CHECK(rise > 3.0f);
        CHECK(shift > 3.0f);
    } else {
        CHECK(rise < 0.1f);
        CHECK(shift < 0.1f);
    }
}

TEST_CASE("the Yeti's swipes strike ten along the hand that swings them, which its scale keys "
          "draw about four times as long",
          "[game][combatant][yeti][node-scale][assets]") {
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    /** A swipe's move, the hand it strikes with, its window and its blow's offset along x. */
    struct Swipe {
        const char* name;
        const char* hand;
        s32 start;
        s32 end;
        f32 along;
    };
    constexpr std::array kSwipes{Swipe{"SWIPEL", "JOINT_4", 20, 24, -10},
                                 Swipe{"SWIPER", "JOINT_13", 24, 29, 10}};
    const Swipe& swiped = kSwipes[GENERATE(0U, 1U)];
    CAPTURE(swiped.name);
    test::FakeRenderDevice device;
    test::CombatantFixture f;
    f.open(device, root, nullptr, {}, 'I');
    REQUIRE(f.spawn("YETI", Vec3{0}, 0));
    Combatant& actor = f.actor;
    const auto swipe = actor.data()->moveNamed(swiped.name);
    REQUIRE(swipe.has_value());
    const MoveDefinition& move = actor.data()->moves()[*swipe];
    REQUIRE(move.colnode == swiped.hand);
    REQUIRE(move.frameStart == swiped.start);
    REQUIRE(move.frameEnd == swiped.end);
    const AttackDefinition* blow = actor.data()->damage(move.damage0);
    REQUIRE(blow != nullptr);
    REQUIRE(blow->type == AttackDefinition::kBlow);
    REQUIRE(blow->offset == Vec3{swiped.along, 0, 0});
    REQUIRE(blow->maxDistance == 13);
    // The hand where the window's second frame puts it, drawn about four times as long along
    // its x.
    const auto struck = static_cast<f32>(swiped.start + 1);
    Combatant::MovePoint point;
    point.frame = struck;
    point.target = 0;
    REQUIRE(actor.setMove(*swipe, point));
    const auto hand = actor.nodeTransform(move.colnode);
    REQUIRE(hand.has_value());
    const Vec3 origin{(*hand)[3]};
    const f32 drawn = glm::length(Vec3{(*hand)[0]});
    CAPTURE(origin.x, origin.y, origin.z, drawn);
    REQUIRE(drawn > 3.5f);
    // A player centred on the hand stands within the reach of thirteen of the blow ten along
    // it, as CritterNodePlayerCollide measures it through GetWorldMat (0x800BB614), and out of
    // reach of one forty along.
    std::array<EnemyView, 1> players{};
    players[0].player = 0;
    players[0].position = origin - Vec3{0, 3, 0};
    point.frame = struck - 1;
    REQUIRE(actor.setMove(*swipe, point));
    f.update(2, 1.0f / 30, players);
    REQUIRE(actor.animation().frame() == Approx(struck));
    const auto blows = actor.takeBlows();
    REQUIRE(blows.size() == 1);
    CHECK(glm::distance(blows[0].origin, origin) == Approx(10).margin(0.01f));
}

TEST_CASE("the Plague Fiend's parts stand where GetWorldMat places them as his body's scale keys "
          "swell it through his splash, fin and spout",
          "[game][combatant][plague][node-scale][assets]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/PBOSS/ANIM.PS2");
    test::FakeRenderDevice device;
    test::CombatantFixture f;
    f.open(device, root, nullptr, {}, 'K');
    REQUIRE(f.spawn("PBOSS", Vec3{0}, 0));
    Combatant& actor = f.actor;
    REQUIRE(actor.childCount() == 0); // every part is the body's own
    const CritterData& data = *actor.data();
    const TreeInfo& tree = *f.assets.tree;
    // BODY1 keys its scale over every hit part through ATCK4 and ATCK10B, BODY1_TORSO01
    // through ATCK10.
    const std::string_view anim = GENERATE("ATCK4", "ATCK10B", "ATCK10");
    std::optional<usize> move;
    for (usize i = 0; i < data.moves().size() && !move; ++i) {
        if (data.moves()[i].anim == anim) {
            move = i;
        }
    }
    REQUIRE(move.has_value());
    const auto sequence = tree.findSequence(anim);
    REQUIRE(sequence.has_value());
    f32 misplaced = 0; ///< the furthest a part's volume stands from GetWorldMat's place
    f32 swell = 0;     ///< the furthest the scale keys would have carried a part from it
    usize necks = 0;   ///< poses NECK_HEAD's volume was seen in
    Vec3 nearest{1e9f};
    Vec3 furthest{0};
    for (s32 frame = 0; frame < tree.sequences[*sequence].frames; ++frame) {
        Combatant::MovePoint point;
        point.frame = static_cast<f32>(frame);
        REQUIRE(actor.setMove(*move, point));
        const auto model = actor.nodeTransform({});
        REQUIRE(model.has_value());
        const TreePose pose = actor.presentedPose(-1);
        for (const auto& volume : actor.bodyTargets()) {
            if (volume.node < 0) {
                continue;
            }
            const auto& part = data.parts()[static_cast<usize>(volume.node)];
            const auto node = tree.findNode(part.node, kCombatantNodeNameLength);
            REQUIRE(node.has_value());
            const Vec3 placed{*model * Vec4{worldMatPlace(tree, pose, *node, part.position), 1}};
            const Vec3 centre = volume.base + Vec3{0, 0.5f * volume.height, 0};
            const s32 parent = tree.nodes[*node].parent;
            const Vec3 offset =
                parent >= 0 ? Mat3{pose.matrices()[static_cast<usize>(parent)]} * part.position
                            : part.position;
            const Vec3 scaled{*model * Vec4{Vec3{pose.matrices()[*node][3]} + offset, 1}};
            misplaced = std::max(misplaced, glm::distance(centre, placed));
            swell = std::max(swell, glm::distance(scaled, placed));
            if (part.node == "NECK_HEAD") {
                ++necks;
                nearest = glm::length(centre) < glm::length(nearest) ? centre : nearest;
                furthest = glm::length(centre) > glm::length(furthest) ? centre : furthest;
            }
        }
    }
    CAPTURE(anim, misplaced, swell, nearest.x, nearest.y, nearest.z, furthest.x, furthest.y,
            furthest.z);
    REQUIRE(necks > 0);
    CHECK(misplaced < 1e-3f);
    CHECK(swell > 1.0f);
}

TEST_CASE("critter crowd contact respects height sweep and directional overlap escape",
          "[combatant][critter-body-contact]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    std::array<Combatant, 2> actors;
    Vec3 obstacle{1, 0, 0};
    f32 seconds = 1.0f / 30;
    bool blocked = true;
    SECTION("inward overlap blocks") {}
    SECTION("another floor does not block") {
        obstacle.y = 20;
        blocked = false;
    }
    SECTION("outward overlap escapes") {
        obstacle.x = -1;
        blocked = false;
    }
    SECTION("fast motion sweeps the peer") {
        obstacle.x = 5;
        seconds = 1;
    }
    REQUIRE(actors[0].spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    REQUIRE(actors[1].spawn(assets, 1, obstacle, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = kKnockOver;
    hit.direction = {1, 0, 0};
    actors[0].hurt(hit);
    actors[0].update(2, seconds, {}, actors);
    CHECK(actors[0].position().x == Approx(blocked ? 0.0f : 10.0f * seconds));
}

TEST_CASE("great ones stop at swarm bodies but golems trample small enemies",
          "[combatant][critter-body-contact][critter-trample]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    std::array swarm{EnemyBody{5, {1, 0, 0}, 1, 2}, EnemyBody{9, {100, 0, 0}, 1, 1}};
    bool touches = true;
    SECTION("small body at the inclusive crush threshold") {}
    SECTION("tall bodies stop even golems") {
        swarm[0].halfHeight = 2.1f;
    }
    SECTION("another floor is clear") {
        swarm[0].centre.y = 20;
        touches = false;
    }
    SECTION("overlapping bodies may separate") {
        swarm[0].centre.x = -1;
        touches = false;
    }
    for (const auto& definition :
         {General::definition(), Golem::definition(), Gargoyle::definition()}) {
        CAPTURE(definition.name);
        if (definition.kind != CombatantKind::General) {
            REQUIRE(definition.breaksItems); // both break the level's items; only one the swarm
        }
        const std::string header = R"({"descriptors":[{"prefix":"BODY","name":")" +
                                   definition.name + R"(","type":)" +
                                   std::to_string(static_cast<s32>(definition.kind)) + "}],";
        writeTextFile(root / "critter" / (definition.name + ".json"), header + R"(
          "types":[{"moveCount":1,"maxHealth":100,"radius":2,"wallRadius":1,"damageScale":7}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":0}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        EnemyScales scales;
        scales.damage = 2;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
        actor.setSwarm(swarm);
        EnemyHit hit;
        hit.damage = 1;
        hit.flags = kKnockOver;
        hit.direction = {1, 0, 0};
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        const bool crush =
            touches && definition.kind == CombatantKind::Golem && swarm[0].halfHeight <= 2;
        const bool blocked = touches && !crush;
        CHECK(actor.position().x ==
              Approx(blocked ? 0 : (10 - definition.knockbackReduction) / 30));
        const auto tramples = actor.takeTramples();
        REQUIRE(tramples.size() == (crush ? 1 : 0));
        if (crush) {
            CHECK(tramples[0].enemy == 5);
            CHECK(tramples[0].damage == 14);
        }
        CHECK(actor.takeTramples().empty());
    }
}

TEST_CASE(
    "golem population delivers trample damage without player credit or a lingering corpse body",
    "[combatant][critter-trample][assets]") {
    const auto native =
        test::assetOrSkip("MONSTERS/RAT/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":2,"wallRadius":1,"damageScale":1000}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":0}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, native, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kRatKind));
    const auto rat = enemies.spawn({.kind = kRatKind, .position = {1, 0, 0}, .placed = true}, {});
    REQUIRE(rat);
    REQUIRE(enemies.movementBodies().size() == 1);
    REQUIRE(enemies.movementBodies()[0].halfHeight <= 2);
    Critters population;
    population.open(device, root, nullptr, {}, 'G');
    const auto golem = population.spawn(CombatantKind::Golem, {}, 0);
    REQUIRE(golem);
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = kKnockOver;
    hit.direction = {1, 0, 0};
    population.hurt(*golem, hit);
    population.update(2, 1.0f / 30, {}, false, {}, &enemies);
    CHECK(population.positionOf(*golem).x > 0);
    CHECK_FALSE(enemies.alive(*rat));
    CHECK(enemies.dying(*rat));
    CHECK(enemies.movementBodies().empty());
    for (const auto& loss : enemies.takeLosses()) {
        CHECK(loss.player == -1);
    }
}

TEST_CASE("golem knockback resistance remains a family rule not a shared actor special case",
          "[game][combatant]") {
    s32 readyInterrupt = 60;
    SECTION("an interruptible stance accepts knockdown") {}
    SECTION("an uninterruptible stance rejects knockdown") {
        readyInterrupt = 0;
    }
    const auto root = familyAssets(readyInterrupt);
    test::FakeRenderDevice device;
    for (const auto& definition : {Golem::definition(), General::definition()}) {
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        EnemyHit hit;
        hit.player = 0;
        hit.damage = 10;
        hit.flags = kKnockOver;
        hit.direction = {1, 0, 0};
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.health() == 90); // refusing a reaction does not prevent the damage
        // Knocked over, it is shoved ten, a golem five less, whatever it was doing
        // (CritterDoKnockback runs every update); only its move depends on the stance.
        const f32 shove = (10.0f - definition.knockbackReduction) / 30;
        REQUIRE(actor.position().x == Approx(shove));
        REQUIRE(actor.moveName() == (readyInterrupt != 0 ? "KD" : "READY"));
        // The push loses a fifth each 30 Hz frame (CritterTranslate), not each tick.
        actor.update(2, 1.0f / 30, {});
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.position().x == Approx(shove * (1 + 0.8 + 0.64)));
    }
}

TEST_CASE("a boss is never shoved by a hit's flags whether standing or falling",
          "[game][combatant][knockback]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    constexpr f32 kKnockOverShove = 10.0f; // what a knock-over pushes a great one
    constexpr f32 kShoveLeft = 0.8f;       // what a frame leaves of a shove
    constexpr f32 kDeathShove = 20.0f;     // what the killing blow adds
    for (const auto& definition : {General::definition(), bossDefinition("LICH")}) {
        CAPTURE(definition.name);
        const bool boss = definition.kind == CombatantKind::Boss;
        // A home radius lets the boss move at all, so only its family keeps the shove off.
        const std::string header = R"({"descriptors":[{"prefix":"BODY","name":")" +
                                   definition.name + R"(","type":)" +
                                   std::to_string(static_cast<s32>(definition.kind)) + "}],";
        writeTextFile(root / "critter" / (definition.name + ".json"), header + R"(
          "types":[{"moveCount":3,"maxHealth":100,"radius":1,"roamRadius":25}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
                   {"name":"KD","anim":"STEP","type":66,"priority":100},
                   {"name":"DEATH","anim":"STEP","type":17,"priority":999}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        EnemyHit hit;
        hit.player = 0;
        hit.damage = 10;
        hit.flags = kKnockOver;
        hit.direction = {1, 0, 0};
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.health() == Approx(90));
        CHECK(actor.position().x == Approx(boss ? 0.0f : kKnockOverShove / 30));
        // Falling, a great one keeps what a frame left of the first shove and is shoved twenty
        // more by the killing blow; a boss falls where it stood.
        const f32 struck = actor.position().x;
        hit.damage = 1000;
        actor.hurt(hit);
        REQUIRE(actor.dying());
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.present());
        CHECK(actor.position().x ==
              Approx(boss ? 0.0f : struck + (kKnockOverShove * kShoveLeft + kDeathShove) / 30));
        CHECK(actor.position().z == 0.0f);
    }
}

TEST_CASE("a family's own policies and not its kind decide how shared combat treats it",
          "[game][combatant][combatant-policy]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    const CombatantDefinition boss = bossDefinition("LICH");
    // The golem's own family, then the same family given the boss's answer to one policy: the
    // golem's kind stays, so only the policy can make it fight as the boss does.
    const auto golems = [](const auto& give) {
        CombatantDefinition given = Golem::definition();
        give(given);
        return std::array{std::pair{Golem::definition(), false}, std::pair{given, true}};
    };
    const auto spawned = [&](CombatantAssets& assets, Combatant& actor,
                             const CombatantDefinition& family, const EnemyScales& scales = {}) {
        REQUIRE(assets.load(device, root, family, 'G'));
        REQUIRE(actor.spawn(assets, 7, {}, 0, nullptr, scales, 'G'));
    };
    SECTION("unshoved, a knock-over leaves it where it stood") {
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.shoved = boss.shoved; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family);
            EnemyHit hit;
            hit.damage = 10;
            hit.flags = kKnockOver;
            hit.direction = {1, 0, 0};
            actor.hurt(hit);
            actor.update(2, 1.0f / 30, {});
            CHECK(actor.position().x ==
                  Approx(given ? 0.0f : (10 - family.knockbackReduction) / 30));
        }
    }
    SECTION("scaled by the party's size, it takes less, pays more and loses nothing to a level") {
        EnemyScales scales;
        scales.players = 3;
        scales.playerLevel = 100;
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.harmScale = boss.harmScale; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family, scales);
            EnemyHit hit;
            hit.player = 0;
            hit.level = 80;
            hit.damage = 20;
            const f32 credited = given ? 6.0f : 20.0f;
            CHECK(actor.hurt(hit) == Approx(credited));
            CHECK(actor.health() == Approx(100.0f - (given ? credited : 12.0f)));
            const auto losses = actor.takeLosses();
            REQUIRE(losses.size() == 1);
            CHECK(losses[0].experience ==
                  Approx(credited / 101.0f * 50.0f * (given ? 3.0f : 1.0f)));
        }
    }
    SECTION("never shrunk, it takes and deals its harm whole") {
        writeTextFile(root / "critter/GOLEM.json", R"({
          "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
          "types":[{"moveCount":2,"maxHealth":100,"radius":3}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
                   {"name":"CLAW","anim":"STEP","type":128,"priority":20,
                    "colnode":"BODY","target":{"maxDistance":50},
                    "frameStart":0,"frameEnd":2,"damage0":0}],
          "damages":[{"type":0,"radius":3,"maxDistance":1,"damage":10,"offset":[0,0,8]}]})");
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.shrinks = boss.shrinks; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            REQUIRE(assets.load(device, root, family, 'G'));
            REQUIRE(actor.spawn(assets, 7, {}, 3.14159265f, nullptr, {}, 'G'));
            actor.setShrink(0.667f);
            EnemyHit hit;
            hit.damage = 12;
            actor.hurt(hit);
            CHECK(actor.health() == Approx(given ? 88.0f : 76.0f));
            // Shrunk or not, it strikes from where its whole body puts the claw.
            EnemyView player;
            player.player = 0;
            player.position = {0, 0, -8};
            player.radius = 0.75f;
            player.height = 5.0f;
            const std::array players{player};
            std::vector<CombatBlow> blows;
            for (s32 i = 0; i < 10 && blows.empty(); ++i) {
                actor.update(2, 1.0f / 30, players);
                blows = actor.takeBlows();
            }
            REQUIRE(blows.size() == 1);
            CHECK(blows[0].damage == Approx(given ? 10.0f : 5.0f));
        }
    }
    SECTION("fading at its death, it leaves no drop") {
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.departure = boss.departure; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family);
            EnemyHit hit;
            hit.damage = 1000;
            hit.direction = Vec3{0};
            actor.hurt(hit);
            REQUIRE(actor.dying());
            usize drops = 0;
            for (s32 frame = 0; frame < 90 && actor.present(); ++frame) {
                actor.update(2, 1.0f / 30, {});
                drops += static_cast<usize>(std::ranges::count_if(
                    actor.takeLosses(), [](const CombatLoss& loss) { return loss.drop; }));
            }
            CHECK_FALSE(actor.present());
            CHECK(drops == (given ? 0 : 1));
        }
    }
    SECTION("ranking by anger, it turns on the player who hit it") {
        writeTextFile(root / "critter/GOLEM.json", R"({
          "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
          "types":[{"moveCount":1,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32}]})");
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.targeting = boss.targeting; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family);
            std::array<EnemyView, 2> players;
            players[0].player = 3;
            players[0].position = {0, 0, 4};
            players[1].player = 1;
            players[1].position = {0, 0, 10};
            actor.update(1, 0.25f, players);
            REQUIRE(actor.target() == 3);
            EnemyHit hit;
            hit.player = 1;
            hit.damage = 10;
            actor.hurt(hit);
            actor.update(1, 0.25f, players);
            CHECK(actor.target() == (given ? 1 : 3));
        }
    }
    SECTION("seeing through invisibility, it takes a player the others cannot see") {
        writeTextFile(root / "critter/GOLEM.json", R"({
          "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
          "types":[{"moveCount":1,"maxHealth":1000}],
          "moves":[{"name":"READY","anim":"STEP","type":32}]})");
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.seesInvisible = boss.seesInvisible; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family);
            std::array<EnemyView, 1> players;
            players[0].player = 1;
            players[0].position = {0, 0, 10};
            players[0].invisible = true;
            actor.update(1, 0.25f, players);
            CHECK(actor.target() == (given ? 1 : -1));
        }
    }
    SECTION("not trampling, it is stopped by a small swarm body") {
        writeTextFile(root / "critter/GOLEM.json", R"({
          "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
          "types":[{"moveCount":1,"maxHealth":100,"radius":2,"wallRadius":1,"damageScale":7}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":0}]})");
        const std::array swarm{EnemyBody{5, {1, 0, 0}, 1, 2}};
        for (const auto& [family, given] :
             golems([&](CombatantDefinition& f) { f.tramplesSwarm = boss.tramplesSwarm; })) {
            CAPTURE(given);
            CombatantAssets assets;
            Combatant actor;
            spawned(assets, actor, family);
            actor.setSwarm(swarm);
            EnemyHit hit;
            hit.damage = 1;
            hit.flags = kKnockOver;
            hit.direction = {1, 0, 0};
            actor.hurt(hit);
            actor.update(2, 1.0f / 30, {});
            CHECK(actor.position().x ==
                  Approx(given ? 0.0f : (10 - family.knockbackReduction) / 30));
            CHECK(actor.takeTramples().size() == (given ? 0 : 1));
        }
    }
}

TEST_CASE("plain harm leaves a great one be; flagged hits shake, knock back or knock it over",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 5;
    hit.direction = {1, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.moveName() == "READY"); // CritterGetDoAction: no flag, no reaction
    CHECK(actor.position().x == 0.0f);
    hit.flags = EnemyHit::kKnockBack; // 0x10 shakes it: five
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    // CritterDoKnockback leaves an unflagged hit's input pending until a positive
    // knockback scale consumes it. Both accepted directions now contribute.
    CHECK(actor.position().x == Approx(10.0f / 30));
}

TEST_CASE("critter knockback accumulates raw hit inputs before clamping the combined velocity",
          "[game][combatant][critter-knockback]") {
    const bool capped = GENERATE(false, true);
    CAPTURE(capped);
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition : {General::definition(), Gargoyle::definition()}) {
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        EnemyHit first;
        first.damage = 1;
        first.flags = EnemyHit::kKnockBack;
        first.direction = {2, 0.5f, 0};
        EnemyHit second = first;
        second.direction = {0, 0, 1};
        if (capped) {
            first.direction = {10, 0, 0};
            second.direction = {0, 0, 10};
        }
        actor.hurt(first);
        actor.hurt(second);
        CHECK(actor.health() == 98);
        Vec3 expected = (first.direction + second.direction) * 5.0f;
        if (const f32 magnitude = glm::length(expected); magnitude > 40.0f) {
            expected *= 40.0f / magnitude;
        }
        actor.update(2, 1.0f / 30, {});
        CHECK(glm::distance(actor.position(), expected / 30.0f) < 0.0001f);
    }
}

TEST_CASE("a directionless critter hit cannot reapply a consumed knockback input",
          "[game][combatant][critter-knockback]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = EnemyHit::kKnockBack;
    hit.direction = {2, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.position().x == Approx(10.0f / 30));
    hit.direction = Vec3{0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.position().x == Approx((10.0f / 30) * (1.0f + 0.8f)));
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.position().x == Approx((10.0f / 30) * (1.0f + 0.8f + 0.64f)));
}

TEST_CASE("a rejected critter node hit cannot change an accepted knockback input",
          "[game][combatant][critter-knockback]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":1,"maxHealth":100,"radius":1,"colCount":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32}],
      "nodes":[{"nodeName":"BODY","radius":1,"healthScale":0.1,"damageScale":1}]
    })");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 10;
    hit.flags = EnemyHit::kKnockBack;
    hit.direction = {1, 0, 0};
    hit.node = 0;
    actor.hurt(hit);
    REQUIRE(actor.health() == Approx(90));
    hit.direction = {0, 0, 100};
    actor.hurt(hit); // The already-exhausted node rejects this hit completely.
    CHECK(actor.health() == Approx(90));
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.position().x == Approx(5.0f / 30));
    CHECK(actor.position().z == 0.0f);
}

TEST_CASE("fractional accepted critter damage still requests knockback and a hit reaction",
          "[game][combatant][critter-knockback]") {
    s32 readyInterrupt = 60;
    SECTION("the authored stance allows the reaction") {}
    SECTION("an uninterruptible stance still accepts the impulse") {
        readyInterrupt = 0;
    }
    const auto root = familyAssets(readyInterrupt);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 0.25f;
    hit.flags = kKnockOver;
    hit.direction = {0.5f, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.health() == Approx(99.75f));
    CHECK(actor.position().x == Approx(5.0f / 30));
    CHECK(actor.moveName() == (readyInterrupt != 0 ? "KD" : "READY"));
}

TEST_CASE("mixed families preserve the shared capacity while boss ownership is independent",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    Critters population;
    population.open(device, root, nullptr, {}, 'G');
    REQUIRE(population.spawnGolem({}, 0) == 0);
    REQUIRE(population.spawnGeneral({10, 0, 0}, 0) == 1);
    REQUIRE(population.spawnGargoyle({20, 0, 0}, 0) == 2);
    for (s32 i = 3; i < Critters::kMost; ++i) {
        REQUIRE(population.spawnGeneral({static_cast<f32>(i) * 10, 0, 0}, 0) == i);
    }
    REQUIRE_FALSE(population.spawnGolem({}, 0).has_value());
    REQUIRE_FALSE(population.spawn(CombatantKind::Boss, {}, 0, "DJINN").has_value());
    Bosses boss;
    boss.open(device, root, nullptr, {}, 'G');
    REQUIRE(boss.spawn(36, {100, 0, 100}, 0));
    REQUIRE(boss.present());
    REQUIRE(population.count() == 16);
    EnemyHit kill;
    kill.player = 0;
    kill.damage = 1000;
    population.hurt(2, kill);
    const auto loss = population.takeLosses();
    REQUIRE(loss.size() == 2);
    REQUIRE(loss[1].killed);
    REQUIRE(loss[1].form == "EAGL");
    REQUIRE(loss[1].kind == CombatantKind::Gargoyle);
    for (s32 frame = 0; frame < 90; ++frame) {
        population.update(2, 1.0f / 30, {});
    }
    REQUIRE(population.count() == 15);
    REQUIRE(population.spawnGeneral({20, 0, 0}, 0) == 2);
    REQUIRE(population.formOf(2).empty());
    REQUIRE(boss.view().health == 100);
    population.close();
    REQUIRE(boss.present());
}

TEST_CASE("a general blocks a golem and damage events preserve submission order",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    Critters population;
    population.open(device, root, nullptr, {}, 'G');
    REQUIRE(population.spawnGolem({}, 0) == 0);
    REQUIRE(population.spawnGeneral({0, 0, 2}, 0) == 1);
    population.hold(1, true);
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 100};
    const std::array<EnemyView, 1> players{player};
    for (s32 frame = 0; frame < 30; ++frame) {
        population.update(2, 1.0f / 30, players);
    }
    REQUIRE(population.moveOf(0) == "WALK");
    REQUIRE(population.positionOf(0) == Vec3{0});
    REQUIRE(population.positionOf(1) == Vec3{0, 0, 2});
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 5;
    population.hurt(1, hit);
    population.hurt(0, hit);
    const auto losses = population.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[0].critter == 1);
    REQUIRE(losses[1].critter == 0);
}

TEST_CASE("a golem walks through a chest, breaks a barrel in its way and is stopped by the rest",
          "[game][combatant][critter-rams]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 100};
    const std::array<EnemyView, 1> players{player};
    Obstacle box;
    box.centre = Vec3{0, 0, 3};
    box.halfAcross = 0.5f;
    box.halfAlong = 0.5f;
    box.height = 4.0f;
    const auto walk = [&](CombatantObstacle item, bool golem) {
        Critters population;
        population.open(device, root, nullptr, {}, 'G');
        REQUIRE((golem ? population.spawnGolem({}, 0) : population.spawnGeneral({}, 0)) == 0);
        const std::array items{item};
        std::vector<CombatantRam> rams;
        for (s32 frame = 0; frame < 90; ++frame) {
            population.update(2, 1.0f / 30, players, false, items);
            const auto taken = population.takeRams();
            rams.insert(rams.end(), taken.begin(), taken.end());
        }
        return std::pair{population.positionOf(0).z, rams};
    };
    const CombatantObstacle chest{.box = box, .kind = CombatantObstacle::Kind::Chest};
    const auto [past, none] = walk(chest, true);
    CHECK(past > 4.0f); // through it
    CHECK(none.empty());

    const CombatantObstacle barrel{
        .box = box, .kind = CombatantObstacle::Kind::Breakable, .id = 7, .health = 5, .armor = 1};
    const auto [held, rams] = walk(barrel, true);
    CHECK(held < 3.0f); // standing, it stops the golem
    REQUIRE_FALSE(rams.empty());
    CHECK(rams[0].id == 7);

    const CombatantObstacle gate{.box = box};
    const auto [stopped, quiet] = walk(gate, true);
    CHECK(stopped < 3.0f);
    CHECK(quiet.empty());

    // A general breaks nothing: a chest and a barrel stop it like anything else.
    CombatantObstacle gone = gate;
    gone.box.solid = false;
    CHECK(walk(gone, false).first > 4.0f); // it does walk there when nothing is in the way
    const auto [generalAtChest, noRam] = walk(chest, false);
    CHECK(generalAtChest < 3.0f);
    const auto [generalAtBarrel, noBlow] = walk(barrel, false);
    CHECK(generalAtBarrel < 3.0f);
    CHECK(noRam.empty());
    CHECK(noBlow.empty());
}

TEST_CASE("boss replacement retains borrowed archives and undrained death events",
          "[game][combatant]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    Bosses boss;
    boss.open(device, root, nullptr, {}, 'G');
    REQUIRE(boss.spawn(36, {}, 0));
    auto* djinnArchive = boss.archive();
    REQUIRE(djinnArchive != nullptr);
    const auto defeat = [&] {
        EnemyHit kill;
        kill.player = 0;
        kill.damage = 1000;
        boss.wake();
        boss.hurt(kill);
        for (s32 frame = 0; frame < 90; ++frame) {
            boss.update(2, 1.0f / 30, {});
        }
        REQUIRE_FALSE(boss.present());
    };
    defeat();
    REQUIRE(boss.spawn(41, {}, 0));
    REQUIRE(boss.archive() != djinnArchive);
    REQUIRE(djinnArchive->loaded());
    const auto losses = boss.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[1].killed);
    REQUIRE(boss.takeLosses().empty());
    defeat();
    REQUIRE(boss.spawn(36, {}, 0));
    REQUIRE(boss.archive() == djinnArchive);
    REQUIRE(boss.view().health == 100);
}
TEST_CASE("bosses load elemental armor from exported retail types", "[game][items][assets]") {
    const auto dragon = test::assetOrSkip("CRITTER/DRAGON.WAD");
    const auto lich = test::assetOrSkip("CRITTER/LICH.WAD");
    const auto chimera = test::assetOrSkip("CRITTER/CHIMERA.WAD");
    CritterData data;
    REQUIRE(data.load(dragon));
    CHECK(data.shieldFlags() == 1);
    REQUIRE(data.load(lich));
    CHECK(data.shieldFlags() == 8);
    REQUIRE(data.load(chimera));
    CHECK(data.shieldFlags() == 0);
}
TEST_CASE("a great one repeats fire damage at quarter seconds and other floor damage every step",
          "[combatant][hazards]") {
    bool fire = false;
    SECTION("felling surfaces keep their native cadence") {}
    SECTION("fire surfaces use the port's repeat gate") {
        fire = true;
    }
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":1000,"radius":1,"wallRadius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
               {"name":"WALK","anim":"STEP","type":52,"priority":10,"speed":3}]})");
    const auto dir = test::scratchDirectory("critter-hazard-floor");
    writeTextFile(dir / "world.json", std::string{R"({
  "objects": [{"name": "EMBERS", "position": [0, 0, 0], "flags": )"} +
                                          std::to_string(fire ? 0x10004 : 0x30004) +
                                          R"(, "next": -1,
               "child": -1}],
  "animations": [], "particles": [], "locators": [], "itemInfos": [], "itemInstances": []
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    HazardSurfaces hazards;
    hazards.bind(layout);
    const Vec3 up{0.0f, 1.0f, 0.0f};
    std::vector<CollisionTriangle> floor(2);
    floor[0].vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{200, 0, -200}};
    floor[1].vertices = {Vec3{-200, 0, -200}, Vec3{-200, 0, 200}, Vec3{200, 0, 200}};
    for (CollisionTriangle& triangle : floor) {
        triangle.normal = up;
        triangle.object = 0;
    }
    WorldCollision collision;
    collision.build(floor);
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    for (const bool harmful : {false, true}) {
        CAPTURE(harmful);
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, &collision, {}, 'G'));
        if (harmful) {
            actor.setHazards(&hazards);
        }
        EnemyView player;
        player.player = 0;
        player.position = {0, 0, 150};
        const std::array players{player};
        const f32 whole = actor.health();
        for (s32 frame = 0; frame < 30 && actor.position().z <= 0.0f; ++frame) {
            actor.update(2, 1.0f / 30, players);
        }
        REQUIRE(actor.position().z > 0.0f);
        actor.update(2, 1.0f / 30, players);
        if (!harmful) {
            CHECK(actor.health() == whole);
            continue;
        }
        CHECK(actor.health() < whole);
        const f32 damage = fire ? 5.0f : 15.0f;
        CHECK(std::fmod(whole - actor.health(), damage) == Approx(0.0f).margin(0.01f));
        f32 previous = actor.health();
        s32 lastHit = -100;
        s32 hits = 0;
        for (s32 frame = 0; frame < 60; ++frame) {
            actor.update(2, 1.0f / 30, players);
            if (actor.health() < previous) {
                if (fire) {
                    CHECK(static_cast<f32>(frame - lastHit) / 30 >= 0.25f);
                }
                previous = actor.health();
                lastHit = frame;
                ++hits;
            }
        }
        CHECK(hits >= 2);
        for (const auto& loss : actor.takeLosses()) {
            CHECK(loss.experience == 0.0f);
        }
    }
}

TEST_CASE("Stop Time allows entrances and their explicit continuation without locomotion",
          "[combatant][stop-time]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":3,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
               {"name":"START","anim":"STEP","type":16,"link":0,"speed":3},
               {"name":"WALK","anim":"STEP","type":52,"priority":10,"speed":3}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    REQUIRE(actor.moveName() == "START");
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 100};
    const std::array players{player};
    for (s32 frame = 0; frame < 60; ++frame) {
        actor.update(2, 1.0f / 30, players, {}, true);
    }
    CHECK(actor.moveName() == "READY");
    CHECK_FALSE(actor.moveDone());
    CHECK(actor.position() == Vec3{0});
    for (s32 frame = 0; frame < 12; ++frame) {
        actor.update(2, 1.0f / 30, players);
    }
    CHECK(actor.moveName() == "WALK");
    CHECK(glm::length(actor.position()) > 0);
}

TEST_CASE("Stop Time holds living ordinary combatants but damage and death still resolve",
          "[combatant][stop-time]") {
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 100};
    const std::array<EnemyView, 1> players{player};
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition()}) {
        CAPTURE(definition.name);
        test::FakeRenderDevice device;
        CombatantAssets assets;
        REQUIRE(assets.load(device, familyAssets(), definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        for (s32 frame = 0; frame < 60; ++frame) {
            actor.update(2, 1.0f / 30, players, {}, true);
        }
        CHECK(actor.moveName() == "READY");
        CHECK_FALSE(actor.moveDone());
        CHECK(actor.position() == Vec3{0});
        for (s32 frame = 0; frame < 12; ++frame) {
            actor.update(2, 1.0f / 30, players);
        }
        const Vec3 walked = actor.position();
        CHECK(glm::length(walked) > 0);
        EnemyHit hit;
        hit.damage = 5;
        actor.hurt(hit);
        CHECK(actor.health() == 95);
        for (s32 frame = 0; frame < 30; ++frame) {
            actor.update(2, 1.0f / 30, players, {}, true);
        }
        CHECK(actor.position() == walked);
        hit.damage = 1000;
        actor.hurt(hit);
        CHECK(actor.dying());
        for (s32 frame = 0; frame < 90; ++frame) {
            actor.update(2, 1.0f / 30, players, {}, true);
        }
        CHECK_FALSE(actor.present());
    }
}

TEST_CASE("shrunk, a great one is drawn at the shrinkers' scale, takes double and deals half; a "
          "boss is drawn, takes and deals as it is",
          "[game][combatant][shrink]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition : {Golem::definition(), bossDefinition("LICH")}) {
        CAPTURE(definition.name);
        const bool boss = definition.kind == CombatantKind::Boss;
        // Beside the one-triangle body, a unit along x, a shadow as long, which its type lays.
        auto archive = root / "MONSTERS" / definition.name;
        if (definition.realmCostume) {
            archive /= "LEVELG";
        }
        writeTextFile(archive / "models/flat.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 0 1\nvn 0 -1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(archive / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1},
          {"index":1,"name":"SHADOW1L1","file":"models/flat.obj","meshTriangles":1}]})");
        test::convertModelFixture(archive);
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      R"({"descriptors":[{"prefix":"BODY","name":")" + definition.name +
                          R"(","type":)" + std::to_string(static_cast<s32>(definition.kind)) +
                          R"(}],
          "types":[{"moveCount":1,"maxHealth":100,"radius":1,"typeFlags":1}],
          "moves":[{"name":"READY","anim":"STEP","type":32}]})");
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        const auto reach = [](const std::vector<test::RecordedDraw>& draws) {
            f32 longest = 0.0f;
            for (const auto& draw : draws) {
                for (const auto& vertex : draw.vertices) {
                    longest = std::max(longest, vertex.position.x);
                }
            }
            return longest;
        };
        const auto drawnLength = [&] {
            device.draws.clear();
            actor.draw(device, Mat4{1.0f}, {});
            REQUIRE_FALSE(device.draws.empty());
            return reach(device.draws);
        };
        const auto shadowLength = [&] {
            device.draws.clear();
            actor.drawShadow(device, Mat4{1.0f}, Vec3{0, 60, 0}, {});
            REQUIRE(device.draws.size() == 1);
            return reach(device.draws);
        };
        CHECK(actor.shrink() == 1.0f);
        actor.setShrink(0.5f);
        CHECK(actor.shrink() == 0.5f);
        CHECK(actor.scale() == 1.0f); // its own size is another matter
        EnemyHit hit;
        hit.player = 2;
        hit.damage = 12;
        actor.hurt(hit);
        CHECK(actor.health() == Approx(boss ? 88.0f : 76.0f));
        // CritterGolemAI sizes the body and its shadow by the shrinkers' scale (0x80039A4C);
        // CritterBossAI never does, so a boss is drawn whole. The shadow, pulled toward the eye,
        // is measured against its whole length.
        CHECK(drawnLength() == Approx(boss ? 1.0f : 0.5f));
        const f32 shrunkShadow = shadowLength();
        actor.setShrink(1.0f);
        CHECK(drawnLength() == Approx(1.0f));
        CHECK(shrunkShadow == Approx((boss ? 1.0f : 0.5f) * shadowLength()));
    }
    // The gargoyle's claw of ten lands for five, from where its whole body puts it.
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"CLAW","anim":"STEP","type":128,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":0,"radius":3,"maxDistance":1,"damage":10,"offset":[0,0,8]}]})");
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {0, 0, 0}, 3.14159265f, nullptr, {}, 'G'));
    actor.setShrink(0.667f);
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, -8};
    player.radius = 0.75f;
    player.height = 5.0f;
    const std::array players{player};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 10 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
    }
    REQUIRE(blows.size() == 1);
    CHECK(blows[0].damage == Approx(5.0f));
    CHECK(blows[0].origin.z == Approx(-8).margin(0.01f));
}

TEST_CASE("a shrunk great one is struck, blocks and strikes where it stands whole, as GetWorldMat "
          "ignores the draw scale",
          "[game][combatant][shrink]") {
    // CritterGolemAI's MBTreeSetScale sizes the drawn body alone (0x80039A4C); its parts, c->pos
    // and its blows' place are measured through GetWorldMat (0x800BB614), which never reads it.
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1,"wallRadius":1,"colCount":2,
                "originOffset":[0,3,5],"target":{"maxDistance":10,"minDot":0.5}}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"CLAW","anim":"STEP","type":128,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "nodes":[{"nodeName":"BODY","position":[5,3,0],"radius":2,"flags":8},
               {"nodeName":"BODY","position":[0,8,0],"radius":1,"flags":0}],
      "damages":[{"type":0,"radius":3,"maxDistance":1,"damage":10,"offset":[0,0,8]}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    constexpr f32 kQuarterTurn = 1.5707963267948966f;
    // A whole golem and one shrunk to two thirds, both at (10, 0, 0) facing +x.
    const auto spawned = [&](std::array<Combatant, 2>& actors) {
        for (auto& actor : actors) {
            REQUIRE(actor.spawn(assets, 7, {10, 0, 0}, kQuarterTurn, nullptr, {}, 'G'));
        }
        actors[1].setShrink(0.667f);
    };
    SECTION("its parts and its body stand where the whole one's do") {
        std::array<Combatant, 2> actors;
        spawned(actors);
        for (const bool solidOnly : {false, true}) {
            CAPTURE(solidOnly);
            const auto whole = actors[0].bodyTargets(solidOnly);
            const auto shrunk = actors[1].bodyTargets(solidOnly);
            REQUIRE(whole.size() == (solidOnly ? 2U : 3U)); // the parts it lists, then the body
            REQUIRE(shrunk.size() == whole.size());
            for (usize i = 0; i < whole.size(); ++i) {
                CAPTURE(i);
                CHECK(shrunk[i].base == whole[i].base);
                CHECK(shrunk[i].radius == whole[i].radius);
                CHECK(shrunk[i].height == whole[i].height);
            }
        }
        // The solid part at its whole offset (5, 3, 0) turned with the body, based its radius
        // of two below that.
        const auto solid = actors[1].bodyTargets(true);
        CHECK(solid[0].base.x == Approx(10));
        CHECK(solid[0].base.y == Approx(1));
        CHECK(solid[0].base.z == Approx(-5));
    }
    SECTION("it sees from its whole origin, five ahead") {
        std::array<Combatant, 2> actors;
        spawned(actors);
        // Within ten of the whole origin at x 15, past ten of a shrunk one's at x 13.3.
        EnemyView player;
        player.player = 2;
        player.position = {24.9f, 0, 0};
        for (auto& actor : actors) {
            actor.update(2, 1.0f / 30, std::array{player});
            CHECK(actor.target() == 2);
        }
    }
    SECTION("its claw lands eight ahead of the node and pushes away from its whole origin") {
        std::array<Combatant, 2> actors;
        spawned(actors);
        EnemyView player;
        player.player = 0;
        player.position = {18, 0, 0};
        player.radius = 0.75f;
        player.height = 5.0f;
        const std::array players{player};
        std::array<std::vector<CombatBlow>, 2> blows;
        for (usize a = 0; a < actors.size(); ++a) {
            CAPTURE(a);
            for (s32 i = 0; i < 10 && blows[a].empty(); ++i) {
                actors[a].update(2, 1.0f / 30, players);
                blows[a] = actors[a].takeBlows();
            }
            REQUIRE(blows[a].size() == 1);
        }
        CHECK(blows[0][0].origin.x == Approx(18));
        CHECK(blows[1][0].origin == blows[0][0].origin);
        CHECK(blows[1][0].direction == blows[0][0].direction);
        CHECK(blows[1][0].damage == Approx(0.5f * blows[0][0].damage));
    }
}

TEST_CASE("a shrunk great one grabs and holds a player where its whole body reaches, as "
          "CritterNodePlayerCollide measures it",
          "[game][combatant][shrink][combatant-grab]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"GRAB","anim":"STEP","type":129,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":7,"radius":3,"maxDistance":1,"damage":10,"offset":[0,0,8]}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    const f32 shrink = GENERATE(1.0f, 0.667f);
    CAPTURE(shrink);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    actor.setShrink(shrink);
    // Eight ahead, where the offset reaches from the whole body; a shrunk one's would fall 2.7
    // short, past the reach of one and the player's width of one.
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 8};
    player.radius = 1.0f;
    player.height = 6.0f;
    const std::array players{player};
    std::vector<CombatGrab> grabs;
    for (s32 i = 0; i < 10 && grabs.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        grabs = actor.takeGrabs();
    }
    REQUIRE(grabs.size() == 1);
    CHECK(grabs[0].player == 0);
    REQUIRE(grabs[0].attachment.has_value());
    CHECK(glm::distance(Vec3{(*grabs[0].attachment)[3]}, Vec3{0, 0, 8}) < 1e-5f);
}

TEST_CASE("a shrunk great one's area starts at its whole node, the offsets laid in it shrunk "
          "once as CritterDoTexmodNode and CritterDoSfx shrink them",
          "[game][combatant][shrink]") {
    // Attached to the node, or placed once where the node stands (SFXX 0x40).
    const u32 soundFlags = GENERATE(0U, 64U);
    CAPTURE(soundFlags);
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/GOLEM/LEVELG/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
               {"name":"HAND","object":"BODY","parent":0,"position":[0,0,6]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"SWEEP","anim":"STEP","type":128,"priority":20,
                "colnode":"HAND","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":2,"damage":40,"flags":32,"radius":0.1,"maxDistance":10,"minDot":-1,
                  "offset":[0,3,0],"sfxIndex":0}],
      "sounds":[{"name":"NULLFX","flags":)" + std::to_string(soundFlags) +
                                                   R"(,"life":0.5,"offset":[0,2,0]}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 8};
    player.radius = 1.0f;
    player.height = 6.0f;
    const std::array players{player};
    // Where the area stands as it starts with the shrinkers' scale at `shrink`, and an update
    // after that scale turns to `then`.
    const auto areaOrigins = [&](f32 shrink, f32 then) {
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        actor.setShrink(shrink);
        std::vector<CombatBlow> blows;
        for (s32 i = 0; i < 10 && blows.empty(); ++i) {
            actor.update(2, 1.0f / 30, players);
            blows = actor.takeBlows();
        }
        REQUIRE(blows.size() == 1);
        REQUIRE(blows[0].area);
        actor.setShrink(then);
        actor.update(2, 1.0f / 30, players);
        const auto later = actor.takeBlows();
        REQUIRE_FALSE(later.empty());
        REQUIRE(later.front().area);
        return std::array{blows[0].origin, later.front().origin};
    };
    // HAND stands six ahead whole or shrunk; the DAMG offset of three and the SFXX offset of two
    // above it shrink with the drawn body as the area starts, and stay so as shrinkers come and
    // go, as the original multiplies them into the effect once.
    for (const Vec3& origin : areaOrigins(1.0f, 0.667f)) {
        CHECK(glm::distance(origin, Vec3{0, 5, 6}) < 1e-5f);
    }
    for (const Vec3& origin : areaOrigins(0.667f, 1.0f)) {
        CHECK(glm::distance(origin, Vec3{0, 5 * 0.667f, 6}) < 1e-5f);
    }
}
TEST_CASE("a shrunk great one breathes, shoots and plants from its whole node, the offsets of "
          "what it starts shrunk as CritterDoTexmodNode shrinks them",
          "[game][combatant][shrink]") {
    // CritterFirePlayerCollide measures the breath through GetWorldMat (0x800BB614), its offset
    // whole; a shot leaves the move node's whole origin and a plant the target's place, their
    // DAMG offset shrunk with the drawn body (0x8003D10C-0x8003D154).
    const f32 shrink = GENERATE(1.0f, 0.667f);
    const std::string kind = GENERATE("breath", "shot", "plant");
    CAPTURE(shrink, kind);
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/GOLEM/LEVELG/animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
               {"name":"HAND","object":"BODY","parent":0,"position":[0,0,6]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
    std::string harm = R"({"type":1,"damage":10,"offset":[0,3,0]})";
    s32 moveType = MoveDefinition::kAttackFrom;
    if (kind == "breath") {
        harm = R"({"type":4,"damage":10,"radius":1,"maxDistance":20,"offset":[0,1,0]})";
    } else if (kind == "plant") {
        harm = R"({"type":8,"damage":10,"flags":)" + std::to_string(damage::kSticky) +
               R"(,"offset":[0,3,0]})";
        moveType = MoveDefinition::kTargetArea;
    }
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"SWEEP","anim":"STEP","type":)" +
                                                   std::to_string(moveType) + R"(,"priority":20,
                "colnode":"HAND","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[)" + harm + "]}");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    actor.setShrink(shrink);
    // Ten along the breath from where it leaves the whole HAND, its middle three up.
    EnemyView player;
    player.player = 0;
    player.position = {0, -2, 16};
    const std::array players{player};
    std::vector<CombatBlow> blows;
    std::vector<CombatShot> shots;
    for (s32 i = 0; i < 10 && blows.empty() && shots.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
        shots = actor.takeShots();
    }
    // What it starts is sized with the drawn body too, its reach as well as its art
    // (0x8003D410-0x8003D424).
    if (kind == "breath") {
        REQUIRE(blows.size() == 1);
        REQUIRE(blows[0].breath);
        CHECK(glm::distance(blows[0].origin, Vec3{0, 1, 6}) < 1e-5f);
    } else if (kind == "shot") {
        REQUIRE(shots.size() == 1);
        CHECK(glm::distance(shots[0].origin, Vec3{0, 3 * shrink, 6}) < 1e-5f);
        CHECK(shots[0].scale == shrink);
    } else {
        REQUIRE(shots.size() == 1);
        CHECK(glm::distance(shots[0].origin, Vec3{0, 1 + 3 * shrink, 16}) < 1e-5f);
        CHECK(shots[0].scale == shrink);
    }
}

TEST_CASE("a shrunk great one's rings and targeted areas reach two thirds as far, as "
          "CritterDoTexmodNode sizes the reach of what it starts by the drawn body",
          "[game][combatant][shrink]") {
    // CritterDoTexmodNode multiplies an effect's damage radius and hit radius by the drawn
    // body's scale, mbnode->scale[1] under flag 8 (0x8003D410-0x8003D424), and ProcessEffects
    // reaches by them. A ring or targeted area with no effect of its own to carry it, which the
    // move's strike measures instead, reaches as such an effect would.
    enum class Harm : u8 { CarriedRing, Ring, TargetedArea };
    const Harm harm = GENERATE(Harm::CarriedRing, Harm::Ring, Harm::TargetedArea);
    const f32 shrink = GENERATE(1.0f, 0.667f);
    CAPTURE(static_cast<s32>(harm), shrink);
    const bool targeted = harm == Harm::TargetedArea;
    const auto root = familyAssets();
    const std::string record =
        R"({"type":)" +
        std::to_string(targeted ? AttackDefinition::kTargetArea : AttackDefinition::kRing) +
        R"(,"damage":40,"flags":32,"radius":3,"maxDistance":10,"minDot":-1)" +
        (harm == Harm::CarriedRing ? R"(,"sfxIndex":0})" : "}");
    writeTextFile(
        root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"STOMP","anim":"STEP","type":)" +
                                         std::to_string(targeted ? MoveDefinition::kTargetArea
                                                                 : MoveDefinition::kAttackFrom) +
                                         R"(,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[)" + record + R"(],
      "sounds":[{"name":"NULLFX","life":1}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    actor.setShrink(shrink);
    // A ring spreads from the golem's feet; the targeted area from the middle of player 0, the
    // nearest, four ahead. Player 1 stands eight out from that centre, past the shrunk reach of
    // 6.67 and the player's half a unit of width, player 2 six and a half out, within it, and
    // player 3 a unit out with its middle 5.5 over the centre, past the shrunk hit radius of two
    // over half the player's height.
    const Vec3 centre = targeted ? Vec3{0, 3, 4} : Vec3{0};
    const std::array<Vec3, 4> feet{Vec3{0, 0, 4}, Vec3{centre.x + 8, 0, centre.z},
                                   Vec3{centre.x - 6.5f, 0, centre.z},
                                   Vec3{centre.x + 1, centre.y + 2.5f, centre.z}};
    // An area's own height grows with its reach, so the raised player is the strike's alone.
    const usize count = harm == Harm::CarriedRing ? 3 : 4;
    std::vector<EnemyView> players;
    for (usize p = 0; p < count; ++p) {
        EnemyView& player = players.emplace_back();
        player.player = static_cast<s32>(p);
        player.position = feet[p];
        player.radius = 0.5f;
        player.collisionRadius = 0.5f;
        player.height = 6.0f;
    }
    std::array<bool, 4> struck{};
    for (s32 i = 0; i < 45; ++i) {
        actor.update(2, 1.0f / 30, players);
        for (const CombatBlow& blow : actor.takeBlows()) {
            struck[static_cast<usize>(blow.player)] = true;
        }
    }
    const bool whole = shrink == 1.0f;
    CHECK(struck[0]);
    CHECK(struck[1] == whole);
    CHECK(struck[2]);
    if (count > 3) {
        CHECK(struck[3] == whole);
    }
}

TEST_CASE("a shrunk great one lays the offset of an effect it places in the world shrunk and "
          "draws the effect shrunk, as CritterDoSfx sizes both",
          "[game][combatant][shrink]") {
    // CritterDoSfx multiplies the SFXX offset by the drawn body's scale (0x8003D8A8-0x8003D8D4)
    // and CritterDoSfxSub the effect's own scale (0x8003DE1C-0x8003DE4C). An area laid out where
    // its node stands or where its target stood is drawn on its own, so its cue carries that
    // scale itself.
    const bool targeted = GENERATE(false, true);
    const f32 shrink = GENERATE(1.0f, 0.667f);
    CAPTURE(targeted, shrink);
    const auto root = familyAssets();
    writeTextFile(
        root / "critter/GOLEM.json",
        R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"STOMP","anim":"STEP","type":)" +
            std::to_string(targeted ? MoveDefinition::kTargetArea : MoveDefinition::kAttackFrom) +
            R"(,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":)" +
            std::to_string(targeted ? AttackDefinition::kTargetArea : AttackDefinition::kRing) +
            R"(,"damage":40,"flags":32,"radius":3,
                  "maxDistance":10,"minDot":-1,"offset":[0,3,0],"sfxIndex":0}],
      "sounds":[{"name":"RING","life":1,"scale":2,"offset":[0,2,0],"flags":)" +
            std::to_string(targeted ? 0U : effect_flags::kDetached) + "}]}");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Golem::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    actor.setShrink(shrink);
    // Close enough for the first update's ring to reach.
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 2};
    const std::array players{player};
    std::vector<CombatBlow> blows;
    std::vector<CombatCue> cues;
    for (s32 i = 0; i < 10 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
        for (CombatCue& cue : actor.takeCues()) {
            if (cue.tree == "RING") {
                cues.push_back(std::move(cue));
            }
        }
    }
    REQUIRE(blows.size() == 1);
    REQUIRE(blows[0].area);
    REQUIRE(cues.size() == 1);
    CHECK_FALSE(cues[0].follows);
    CHECK(cues[0].scale == Approx(2 * shrink));
    // Placed once over the golem's feet, or over the middle of the target, three up, the DAMG
    // offset of three and the SFXX offset of two shrunk.
    const Vec3 origin = targeted ? Vec3{0, 3 + 5 * shrink, 2} : Vec3{0, 5 * shrink, 0};
    CHECK(glm::distance(blows[0].origin, origin) < 1e-5f);
    CHECK(glm::distance(cues[0].position, origin) < 1e-5f);
}

TEST_CASE("a great one's look measures from its whole head as GetWorldMat places it, shrunk or "
          "keyed to grow",
          "[game][combatant][gaze][shrink][node-scale]") {
    // HEAD stands eight over the body. NodeLookAtPos measures through GetWorldMat (0x800BB614),
    // which reads neither the shrinkers' draw scale nor the body's scale key, so the head looks
    // down at a player ten ahead from eight up whichever it is; data whose rules keep the scale
    // keys look from where the key draws the head, sixteen up.
    enum class Body : u8 { Whole, Shrunk, Keyed, KeyedKept };
    const Body body = GENERATE(Body::Whole, Body::Shrunk, Body::Keyed, Body::KeyedKept);
    CAPTURE(static_cast<s32>(body));
    const bool keyed = body == Body::Keyed || body == Body::KeyedKept;
    const auto root = familyAssets();
    auto archive = root / "MONSTERS" / General::definition().name;
    if (General::definition().realmCostume) {
        archive /= "LEVELG";
    }
    const std::string grows =
        keyed ? R"(,"tracks":[{"node":0,"flags":1792,"frames":[0],"values":[2,2,2]}])" : "";
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY","nodes":[
      {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
      {"name":"HEAD","object":"BODY","parent":0,"position":[0,8,0]}],
      "sequences":[{"name":"STEP","frames":3)" + grows +
                                                   "}]}]}");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    formats::CritterFile source;
    source.descriptors.push_back({"general", "BODY", static_cast<s16>(CombatantKind::General)});
    auto& type = source.types.emplace_back();
    type.radius = 1;
    type.wallRadius = 1;
    type.maxHealth = 100;
    type.moveCount = 1;
    type.lookNode0 = "HEAD";
    type.lookYawRate0 = 1.0f;
    type.lookPitchRate0 = 1.5f;
    auto& ready = source.moves.emplace_back();
    ready.name = "READY";
    ready.anim = "STEP";
    ready.type = MoveDefinition::kReady;
    ready.target.maxDistance = 50;
    CritterRules rules;
    rules.portRules = body == Body::KeyedKept ? CritterRules::kScaledNodes : 0;
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    if (body == Body::Shrunk) {
        actor.setShrink(0.667f);
    }
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 10};
    const std::array players{player};
    for (s32 i = 0; i < 60; ++i) {
        actor.update(2, 1.0f / 30, players);
    }
    const auto head = actor.nodeTransform("HEAD");
    REQUIRE(head.has_value());
    const Vec3 facing = glm::normalize(Vec3{(*head)[2]});
    const f32 pitch = std::atan2(-facing.y, std::hypot(facing.x, facing.z));
    // Down to the player's middle, three up: five below the head at eight, thirteen at sixteen.
    CHECK(pitch == Approx(std::atan2(body == Body::KeyedKept ? 13.0f : 5.0f, 10.0f)).margin(1e-3f));
}

TEST_CASE("a shrunk great one's swing pushes as a whole one's, its move node's travel measured "
          "whole",
          "[game][combatant][shrink][blow-push]") {
    // The general walks half a unit a frame ahead through SWING as its HAND goes a unit a frame
    // along x. CritterMoveSetup notes the move node through GetWorldMat (0x8003B1CC), which never
    // reads the shrinkers' draw scale, so a shrunk one's node swings as far out for each step.
    test::FakeRenderDevice device;
    CombatantAssets assets;
    const auto root = familyAssets();
    writeSwingTree(root, 0, {},
                   R"(,"tracks":[{"node":2,"flags":16,"frames":[0,10],"values":[0,10]}])");
    REQUIRE(assets.load(device, root, General::definition(), 'G'));
    auto source = swingCritter(4);
    source.moves[1].speed = 15;
    assets.data = CritterData::fromDecoded("GENERAL", source, 0, {});
    EnemyView player;
    player.player = 0;
    player.position = {12, 0, 2};
    const std::array players{player};
    std::array<Combatant, 2> actors;
    std::array<std::vector<CombatBlow>, 2> blows;
    for (usize a = 0; a < actors.size(); ++a) {
        CAPTURE(a);
        REQUIRE(actors[a].spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        actors[a].setShrink(a == 0 ? 1.0f : 0.667f);
        for (s32 i = 0; i < 30 && blows[a].empty(); ++i) {
            actors[a].update(2, 1.0f / 30, players);
            blows[a] = actors[a].takeBlows();
        }
        REQUIRE(blows[a].size() == 1);
    }
    CHECK(glm::distance(blows[1][0].origin, blows[0][0].origin) < 1e-5f);
    CHECK(glm::distance(blows[1][0].direction, blows[0][0].direction) < 1e-5f);
    // The step ahead swings the push that way as well as along the hand's travel.
    CHECK(blows[0][0].direction.z > 0.5f);
}

// Long sequences let these tests distinguish an interrupt from ordinary end-of-move selection.
std::filesystem::path reactionAssets(s32 interrupt = 60, s32 link = -1) {
    const auto root = familyAssets();
    writeTextFile(root / "MONSTERS/GENERAL/LEVELG/animations.json", R"({"trees":[{
      "name":"BODY","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":120}]}]})");
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GENERAL","type":8}],
      "types":[{"moveCount":6,"maxHealth":1000,"radius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":)" +
                                                     std::to_string(interrupt) + R"(,"link":)" +
                                                     std::to_string(link) + R"(},
        {"name":"BLOCK","anim":"STEP","type":35,"interrupt":60,
         "sfx":0,"sfxFrame":1000},
        {"name":"KD","anim":"STEP","type":66,"priority":3840,"interrupt":60},
        {"name":"FLINCH","anim":"STEP","type":64,"priority":3840,"interrupt":60},
        {"name":"ROAR","anim":"STEP","type":34,"priority":3840,"interrupt":60},
        {"name":"DEATH","anim":"STEP","type":17,"priority":4096}],
      "sounds":[{"levelFormat":"BLOCKED"}]})");
    return root;
}

TEST_CASE("critical moves bypass priority but never a locked interrupt policy",
          "[combatant][hit-feedback]") {
    MoveDefinition current;
    current.priority = 5000;
    MoveDefinition candidate;
    candidate.priority = MoveDefinition::kCutsIn;
    for (const s32 policy : {0, 20, 40, 60, 80, 90}) {
        CAPTURE(policy);
        current.interrupt = policy;
        CHECK(candidate.interrupts(current) == (policy != 0));
    }
    candidate.priority = MoveDefinition::kCutsIn - 1;
    current.interrupt = 40;
    CHECK_FALSE(candidate.interrupts(current));
}

TEST_CASE("roar damage scales with party size and expires after a quiet interval",
          "[combatant][hit-feedback]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, reactionAssets(), General::definition(), 'G'));
    constexpr std::array kThresholds{50.0f, 75.0f, 100.0f, 100.0f};
    for (s32 players = 1; players <= 4; ++players) {
        CAPTURE(players);
        const f32 threshold = kThresholds[static_cast<usize>(players - 1)];
        CHECK(Combatant::roarThreshold(players) == threshold);
        Combatant actor;
        EnemyScales scales;
        scales.players = players;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
        std::array<EnemyView, 4> views;
        for (usize i = 0; i < views.size(); ++i) {
            views[i].player = static_cast<s32>(i);
            views[i].position = {0, 0, 100};
        }
        const auto active = std::span<const EnemyView>{views}.first(static_cast<usize>(players));
        EnemyHit hit;
        hit.damage = threshold - 1;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, active);
        CHECK(actor.moveName() == "READY");
        hit.damage = 1;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, active);
        CHECK(actor.moveName() == "ROAR");
    }
    SECTION("old harm is not banked forever") {
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        EnemyHit hit;
        hit.damage = 49;
        actor.hurt(hit);
        actor.update(182, 182.0f / 60, {});
        hit.damage = 1;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.moveName() == "READY");
    }
    SECTION("a new hit refreshes the three second interval") {
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        EnemyHit hit;
        hit.damage = 24;
        actor.hurt(hit);
        actor.update(120, 2, {});
        actor.hurt(hit);
        actor.update(120, 2, {});
        hit.damage = 2;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.moveName() == "ROAR");
    }
}

TEST_CASE("combatant roar thresholds recount standing slots without requiring another hit",
          "[combatant][multiplayer-targeting][combatant-active-count]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, reactionAssets(), General::definition(), 'G'));
    Combatant actor;
    EnemyScales scales;
    scales.players = 4;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, scales, 'G'));
    std::array<EnemyView, 4> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].player = static_cast<s32>(players.size() - i - 1);
        players[i].position = {0, 0, 100};
    }
    actor.update(2, 1.0f / 30, players);
    EnemyHit hit;
    hit.damage = 60;
    hit.player = 3;
    actor.hurt(hit);
    const auto losses = actor.takeLosses();
    REQUIRE(losses.size() == 1);
    CHECK(losses.front().experience ==
          Approx(60.0f / (1.0f + actor.maxHealth()) * actor.data()->experience()));
    actor.update(2, 1.0f / 30, players);
    REQUIRE(actor.moveName() == "READY"); // four standing requires 100 raw damage
    SECTION("only sparse player three remains standing") {
        actor.update(2, 1.0f / 30, std::span<const EnemyView>{players}.first(1));
    }
    SECTION("none standing uses the table's zero slot") {
        actor.update(2, 1.0f / 30, {});
    }
    CHECK(actor.moveName() == "ROAR"); // both zero and one use 50, not the initial 100
}

TEST_CASE("a current reaction consumes incoming reaction flags instead of chaining flinches",
          "[combatant][hit-feedback]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, reactionAssets(), General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 5;
    hit.flags = 0x100;
    hit.direction = Vec3{1, 0, 0};
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    REQUIRE(actor.moveName() == "KD");
    hit.flags = 0x10;
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.moveName() == "KD");
    CHECK(actor.health() == 990);
    CHECK(actor.position().x == Approx((10.0f + 10.0f * 0.8f + 5.0f) / 30));
}

TEST_CASE("linked moves take precedence over hit reactions and block cues wait for impact",
          "[combatant][hit-feedback]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, reactionAssets(60, 1), General::definition(), 'G'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    EnemyHit hit;
    hit.damage = 8;
    hit.flags = 0x100;
    actor.hurt(hit);
    actor.update(2, 1.0f / 30, {});
    // Policy 60 permits the equal-priority linked BLOCK immediately, not KD.
    REQUIRE(actor.moveName() == "BLOCK");
    REQUIRE(actor.takeCues().empty());
    actor.hurt(hit);
    auto cues = actor.takeCues();
    REQUIRE(cues.size() == 1);
    CHECK(cues.front().sound == "BLOCKED");
    actor.hurt(hit);
    CHECK(actor.takeCues().empty());
    CHECK(actor.health() == 988); // eight unblocked, two quarters of eight blocked
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.moveName() == "READY"); // BLOCK also allows the equal-priority ready move.
}

TEST_CASE("heavy hit skin is brief full bright and cannot leak through the shared model",
          "[combatant][hit-feedback]") {
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, familyAssets(), General::definition(), 'G'));
    Combatant actor;
    Combatant sibling;
    REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
    REQUIRE(sibling.spawn(assets, 1, {}, 0, nullptr, {}, 'G'));
    test::FakeTexture flash{1, 1};
    test::FakeTexture frozen{1, 1};
    WorldLighting dark;
    dark.ambient = Vec3{0};
    dark.lightColor = Vec3{0};
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = 0x100000;
    actor.hurt(hit);
    REQUIRE(actor.flashing());
    actor.draw(device, Mat4{1}, dark, nullptr, nullptr, &flash);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().state.maskedTexture == &flash);
    CHECK(device.draws.front().state.alphaToCoverage);
    CHECK(device.draws.front().vertices.front().color == Color::white());
    device.draws.clear();
    sibling.draw(device, Mat4{1}, dark, nullptr, nullptr, &flash);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().state.maskedTexture == nullptr);
    actor.update(2, 1.0f / 30, {});
    CHECK(actor.flashing());
    actor.update(2, 1.0f / 30, {});
    CHECK_FALSE(actor.flashing());
    device.draws.clear();
    actor.draw(device, Mat4{1}, dark, nullptr, nullptr, &flash);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().state.maskedTexture == nullptr);
    hit.flags |= 0x1000000; // suppressed hit visuals
    actor.hurt(hit);
    CHECK_FALSE(actor.flashing());
    hit.flags = 0x100000;
    actor.hurt(hit);
    actor.freeze(300);
    device.draws.clear();
    actor.draw(device, Mat4{1}, dark, &frozen, nullptr, &flash);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(device.draws.front().state.maskedTexture == &frozen);
    CHECK(device.draws.front().state.alphaToCoverage);
    hit.damage = 1000;
    sibling.hurt(hit);
    CHECK_FALSE(sibling.alive());
    CHECK_FALSE(sibling.flashing());
}
} // namespace
