#include <array>
#include <filesystem>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path familyAssets(s32 readyInterrupt = 60) {
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
          "types":[{"moveCount":5,"maxHealth":100,"radius":1,"expValue":50}],
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
        hit.flags = EnemyHit::kKnockDown;
        hit.direction = {1, 0, 0};
        actor.hurt(hit);
        actor.update(2, 1.0f / 30, {});
        REQUIRE(actor.health() == 90); // refusing a reaction does not prevent the damage
        if (readyInterrupt != 0) {
            REQUIRE(actor.moveName() == "KD");
            REQUIRE(actor.position().x == Approx((20.0f - definition.knockbackReduction) / 30));
        } else {
            REQUIRE(actor.moveName() == "READY");
            REQUIRE(actor.position().x == 0);
        }
    }
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
} // namespace
