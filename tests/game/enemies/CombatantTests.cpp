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
#include "game/enemies/Bosses.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
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
TEST_CASE("bosses load elemental armor from exported retail types", "[game][items][unpacked]") {
    const auto dragon = test::unpackedOrSkip("critter/DRAGON.json");
    const auto lich = test::unpackedOrSkip("critter/LICH.json");
    const auto chimera = test::unpackedOrSkip("critter/CHIMERA.json");
    CritterData data;
    REQUIRE(data.load(dragon));
    CHECK(data.shieldFlags() == 1);
    REQUIRE(data.load(lich));
    CHECK(data.shieldFlags() == 8);
    REQUIRE(data.load(chimera));
    CHECK(data.shieldFlags() == 0);
}
TEST_CASE("a great one walking a harmful floor is hurt every step, for nobody's credit",
          "[combatant][hazards]") {
    const auto root = familyAssets();
    writeTextFile(root / "critter/GOLEM.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GOLEM","type":3}],
      "types":[{"moveCount":2,"maxHealth":1000,"radius":1,"wallRadius":1}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
               {"name":"WALK","anim":"STEP","type":52,"priority":10,"speed":3}]})");
    const auto dir = test::scratchDirectory("critter-hazard-floor");
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "EMBERS", "position": [0, 0, 0], "flags": 196612, "next": -1,
               "child": -1}],
  "animations": [], "particles": [], "locators": [], "itemInfos": [], "itemInstances": []
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    HazardSurfaces hazards;
    hazards.bind(layout); // 0x30000: a felling kind, fifteen to the swarm and great ones
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
        CHECK(std::fmod(whole - actor.health(), 15.0f) == Approx(0.0f).margin(0.01f));
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
        EnemyHit hit;
        hit.damage = threshold - 1;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        CHECK(actor.moveName() == "READY");
        hit.damage = 1;
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
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
    CHECK(actor.moveName() == "READY");
    actor.update(240, 4, {});
    actor.update(2, 1.0f / 30, {});
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
    CHECK(actor.moveName() == "BLOCK");
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
