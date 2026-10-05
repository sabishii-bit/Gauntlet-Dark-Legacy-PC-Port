#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
#include "game/enemies/HeadedBodyFixture.h"
#include "game/world/HazardSurfaces.h"

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

TEST_CASE("ready step cooldown includes its animation before the rest interval",
          "[combatant][critter-ready]") {
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
    CHECK(actor.moveName() == "READY");
    for (s32 frame = 0; frame < 35 && actor.moveName() != "WALK"; ++frame) {
        actor.update(2, 1.0f / 30, players);
    }
    CHECK(actor.moveName() == "WALK");
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

TEST_CASE("boss contact windows skip protected attacks without spending their hit",
          "[combatant][attack-invulnerability]") {
    const auto root = familyAssets();
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
          "[combatant][critter-body-contact]") {
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
    CHECK(pushes.size() == (blocked ? 1 : 0));
    if (blocked) {
        CHECK(pushes.front().player == 3);
        CHECK(glm::length(pushes.front().velocity) >= 2.0f);
        CHECK(glm::length(pushes.front().velocity) <= 6.0f);
    }
    CHECK(actor.takePushes().empty());
}

TEST_CASE("node-based critters use solid animated parts for player movement contact",
          "[combatant][critter-body-contact]") {
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
    for (const auto& definition : {General::definition(), Golem::definition()}) {
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
    CHECK(actor.position().x == Approx(5.0f / 30));
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
          "boss takes and deals as it is",
          "[game][combatant][shrink]") {
    const auto root = familyAssets();
    test::FakeRenderDevice device;
    for (const auto& definition : {Golem::definition(), bossDefinition("LICH")}) {
        CAPTURE(definition.name);
        const bool boss = definition.kind == CombatantKind::Boss;
        CombatantAssets assets;
        REQUIRE(assets.load(device, root, definition, 'G'));
        Combatant actor;
        REQUIRE(actor.spawn(assets, 0, {}, 0, nullptr, {}, 'G'));
        CHECK(actor.shrink() == 1.0f);
        actor.setShrink(0.5f);
        CHECK(actor.shrink() == 0.5f);
        CHECK(actor.scale() == 1.0f); // its own size is another matter
        EnemyHit hit;
        hit.player = 2;
        hit.damage = 12;
        actor.hurt(hit);
        CHECK(actor.health() == Approx(boss ? 88.0f : 76.0f));
        // The one-triangle body, a unit along x, is drawn half a unit long.
        device.draws.clear();
        actor.draw(device, Mat4{1.0f}, {});
        REQUIRE_FALSE(device.draws.empty());
        f32 longest = 0.0f;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                longest = std::max(longest, vertex.position.x);
            }
        }
        CHECK(longest == Approx(0.5f));
        actor.setShrink(1.0f);
        device.draws.clear();
        actor.draw(device, Mat4{1.0f}, {});
        longest = 0.0f;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                longest = std::max(longest, vertex.position.x);
            }
        }
        CHECK(longest == Approx(1.0f));
    }
    // The gargoyle's claw of ten lands for five, from where its shrunken body puts it.
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
    player.position = {0, 0, -8 * 0.667f};
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
    CHECK(blows[0].origin.z == Approx(-8 * 0.667f).margin(0.01f));
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
    hit.damage = 1000;
    sibling.hurt(hit);
    CHECK_FALSE(sibling.alive());
    CHECK_FALSE(sibling.flashing());
}
} // namespace
