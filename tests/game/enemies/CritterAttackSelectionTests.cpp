#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/CombatantFixture.h"
#include "game/enemies/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path attackTable(std::string_view table, s32 frames = 3) {
    const auto root = test::scratchDirectory("boss-attack-selection");
    const auto archive = root / "MONSTERS/DJINN";
    std::filesystem::create_directories(root / "critter");
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
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"DJINN",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":)" + std::to_string(frames) +
                                                   R"(}]}]})");
    writeTextFile(root / "critter/DJINN.json", std::string(table));
    return root;
}

EnemyView targetAt(f32 z) {
    EnemyView view;
    view.player = 0;
    view.position = Vec3{0, 0, z};
    view.height = 6;
    view.radius = 1;
    return view;
}

TEST_CASE("ordinary attacks interrupt only when the current move permits their priority",
          "[game][boss-attacks][attack-interruption]") {
    for (const s32 policy : {0, 20, 40, 60, 80, 90}) {
        CAPTURE(policy);
        const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
          "types":[{"moveCount":3,"maxHealth":100}],"moves":[
          {"name":"READY","anim":"STEP","type":32,"priority":0,"interrupt":90},
          {"name":"FAR","anim":"STEP","type":128,"priority":512,"interrupt":)" +
                                          std::to_string(policy) + R"(,"target":{"minDistance":10}},
          {"name":"CLOSE","anim":"STEP","type":128,"priority":513,
           "frameStart":15,"frameEnd":15,"damage0":0,"target":{"maxDistance":9}}],
          "damages":[{"type":1,"sfxIndex":0}],"sounds":[{"name":"SHOT"}]})",
                                      60);
        test::FakeRenderDevice device;
        test::CombatantFixture fixture;
        fixture.open(device, root, nullptr, {}, 'C');
        REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
        std::array party{targetAt(20)};
        fixture.update(2, 1.0f / 30, party);
        REQUIRE(fixture.actor.moveName() == "FAR");
        party[0] = targetAt(5);
        fixture.update(2, 1.0f / 30, party);
        const bool permitted = policy != 0 && policy != 20;
        CHECK(fixture.actor.moveName() == (permitted ? "CLOSE" : "FAR"));
        CHECK(fixture.actor.takeShots().empty());
        // A blocked attempt consumes no cooldown and takes over after completion.
        for (s32 frame = 0; frame < 61; ++frame) {
            fixture.update(2, 1.0f / 30, party);
        }
        CHECK(fixture.actor.moveName() == "CLOSE");
        if (!permitted) {
            // The locked FAR used its full sixty frames before CLOSE started.
            for (s32 frame = 0; frame < 15; ++frame) {
                fixture.update(2, 1.0f / 30, party);
            }
        }
        CHECK(fixture.actor.takeShots().size() == 1);
    }
}

TEST_CASE("queued pattern steps cannot skip a locked running animation",
          "[game][boss-attacks][attack-interruption]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":3,"patternCount":1,"maxHealth":100}],"moves":[
      {"name":"READY","anim":"STEP","type":32,"interrupt":90},
      {"name":"FIRST","anim":"STEP","type":128,"priority":512,"interrupt":0,"flags":4},
      {"name":"SECOND","anim":"STEP","type":128,"priority":1024,"flags":4}],
      "patterns":[{"moves":[1,2],"cooldown":100}]})",
                                  60);
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    const std::array party{targetAt(8)};
    for (s32 frame = 0; frame < 59; ++frame) {
        fixture.update(2, 1.0f / 30, party);
        REQUIRE(fixture.actor.moveName() == "FIRST");
    }
    fixture.update(4, 2.0f / 30, party);
    fixture.update(2, 1.0f / 30, party);
    CHECK(fixture.actor.moveName() == "SECOND");
}

TEST_CASE("an animation hold blocks ordinary cut-ins but not critical priority",
          "[game][boss-attacks][attack-interruption]") {
    for (const s32 priority : {513, MoveDefinition::kCutsIn}) {
        CAPTURE(priority);
        const auto root =
            attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
          "types":[{"moveCount":3,"maxHealth":100}],"moves":[
          {"name":"READY","anim":"STEP","type":32,"interrupt":90},
          {"name":"FAR","anim":"STEP","type":128,"priority":512,"interrupt":90,
           "hold":1,"target":{"minDistance":10}},
          {"name":"CLOSE","anim":"STEP","type":128,"priority":)" +
                            std::to_string(priority) + R"(,"target":{"maxDistance":9}}]})",
                        60);
        test::FakeRenderDevice device;
        test::CombatantFixture fixture;
        fixture.open(device, root, nullptr, {}, 'C');
        REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
        std::array party{targetAt(20)};
        fixture.update(2, 1.0f / 30, party);
        REQUIRE(fixture.actor.moveName() == "FAR");
        party[0] = targetAt(5);
        fixture.update(2, 1.0f / 30, party);
        CHECK(fixture.actor.moveName() == (priority >= MoveDefinition::kCutsIn ? "CLOSE" : "FAR"));
    }
}

TEST_CASE("boss health gates use exclusive upper bounds only above the lower bound",
          "[game][boss-attacks]") {
    TargetCriteria target;
    target.minRateScale = 1.5f;
    target.maxRateScale = 2.5f;
    REQUIRE_FALSE(target.allowsPhase(1.49f, 0));
    REQUIRE(target.allowsPhase(1.5f, 0));
    REQUIRE_FALSE(target.allowsPhase(2.5f, 0));
    target.maxRateScale = target.minRateScale; // Dragon SPLITFBALL is uncapped, not empty.
    REQUIRE(target.allowsPhase(5, 0));
    target.maxHomeDistance = 10;
    REQUIRE(target.allowsPhase(5, 10));
    REQUIRE_FALSE(target.allowsPhase(5, 10.01f));
}

TEST_CASE(
    "Dragon claws stay inside their authored range while distant players receive ranged attacks",
    "[game][boss-attacks][assets][dragon-ranges]") {
    const auto root = test::assetOrSkip("CRITTER/DRAGON.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/DRAGON/ANIM.PS2");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'B');
    for (const f32 distance : {15.0f, 25.0f, 45.0f}) {
        REQUIRE(fixture.spawn("DRAGON", Vec3{0}, 0));
        const std::array party{targetAt(distance)};
        bool attacked = false;
        bool clawed = false;
        std::string previous;
        for (s32 frame = 0; frame < 30 * 90; ++frame) {
            fixture.update(2, 1.0f / 30, party);
            const std::string current(fixture.actor.moveName());
            if (current != previous) {
                const bool claw = current == "CLAWL" || current == "CLAWR";
                CAPTURE(distance, current);
                CHECK_FALSE((claw && distance > 20));
                clawed = clawed || claw;
                attacked = attacked || fixture.actor.moveType() >= MoveDefinition::kAttackFrom;
                previous = current;
            }
        }
        CHECK(attacked);
        if (distance < 20) {
            CHECK(clawed);
        }
    }
}

TEST_CASE("held players carry into the first other eligible grab despite cooldown and range",
          "[game][boss-attacks][combatant-grab]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":5,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"GRAB","anim":"STEP","type":129,"priority":10,"colnode":"BODY",
         "frameStart":0,"frameEnd":1,"damage0":0},
        {"name":"DISABLED_GRAB","anim":"STEP","type":129,"flags":4},
        {"name":"MISSING_NODE_GRAB","anim":"STEP","type":129,"flags":16,"colnode":"MISSING"},
        {"name":"GRABAGAIN","anim":"STEP","type":129,"priority":10,"cooldown":100,
         "colnode":"BODY","frameStart2":2,"frameEnd2":2,"damage1":0,
         "target":{"minDistance":1000,"minRateScale":4}}],
      "damages":[{"type":7,"maxDistance":5,"damage":50,"minSpeed":5}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    const std::vector<EnemyView> party{targetAt(2)};
    fixture.update(6, 0.1f, party); // READY ends
    fixture.update(2, 1.0f / 30, party);
    REQUIRE(fixture.actor.moveName() == "GRAB");
    REQUIRE(fixture.actor.takeGrabs().front().attachment.has_value());
    fixture.update(4, 2.0f / 30, party);
    fixture.actor.takeGrabs();
    fixture.update(2, 1.0f / 30, party);
    REQUIRE(fixture.actor.moveName() == "GRABAGAIN");
    const auto held = fixture.actor.takeGrabs();
    REQUIRE(held.size() == 1);
    REQUIRE(held.front().attachment.has_value());
    fixture.update(2, 1.0f / 30, party);
    const auto released = fixture.actor.takeGrabs();
    REQUIRE_FALSE(released.back().attachment.has_value());
    REQUIRE(released.back().damage == 50);
}

TEST_CASE("boss attack rotation does not starve equal-priority attacks", "[game][boss-attacks]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":4,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"A","anim":"STEP","type":128,"priority":10},
        {"name":"B","anim":"STEP","type":128,"priority":10},
        {"name":"LINK_ONLY","anim":"STEP","type":128,"priority":100,"flags":4}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    const Combatant& critters = fixture.actor;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    REQUIRE(critters.present());
    const std::vector<EnemyView> party{targetAt(8)};
    fixture.update(6, 0.1f, party);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "A");
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "B");
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "A");
}

TEST_CASE("boss chains gate entry not continuation and preserve repeated moves",
          "[game][boss-attacks]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":4,"patternCount":1,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"A","anim":"STEP","type":128,"priority":10},
        {"name":"B","anim":"STEP","type":128,"priority":10,
         "target":{"minRateScale":4}},
        {"name":"OTHER","anim":"STEP","type":128,"priority":10}],
      "patterns":[{"moves":[1,2,2,-1,3],"target":{"minRateScale":0.5,"maxRateScale":1}}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    const Combatant& critters = fixture.actor;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    REQUIRE(critters.present());
    const std::vector<EnemyView> party{targetAt(8)};
    fixture.update(6, 0.1f, party);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "A");
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "B");
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "B");
    fixture.update(6, 0.1f, party);
    // A pattern has its own timestamp; it does not consume its members' solo cooldowns.
    REQUIRE(critters.moveName() == "A");
}

TEST_CASE("boss health opens new moves and evaluates each player's eligibility",
          "[game][boss-attacks]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":3,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"HEALTHY","anim":"STEP","type":128,"priority":10,
         "target":{"maxRateScale":2}},
        {"name":"WOUNDED","anim":"STEP","type":128,"priority":10,
         "frameStart":0,"frameEnd":0,"damage0":0,
         "target":{"minRateScale":2,"minDistance":10}}],
      "damages":[{"type":1,"minSpeed":20,"maxSpeed":40,"sfxIndex":0}],
      "sounds":[{"name":"SHOT"}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    Combatant& critters = fixture.actor;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    REQUIRE(critters.present());
    std::vector<EnemyView> party{targetAt(5), targetAt(20)};
    party[1].player = 1;
    fixture.update(6, 0.1f, party);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "HEALTHY");
    REQUIRE(critters.health() == 100);
    EnemyHit hit;
    // Two standing players halve boss damage. Deal 100 to reach the 50-health
    // phase; this test exercises eligibility, not single-player damage scaling.
    hit.damage = 100;
    critters.hurt(hit);
    REQUIRE(critters.health() == 50);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.moveName() == "WOUNDED");
    const auto shots = critters.takeShots();
    REQUIRE(shots.size() == 1);
    REQUIRE(shots.front().rate > 2);
    REQUIRE(shots.front().target.has_value());
    REQUIRE(shots.front().target->z == 20);
}

TEST_CASE("single-frame boss stomps survive coarse updates and deaths keep authored holds",
          "[game][boss-attacks]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":3,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"STOMP","anim":"STEP","type":128,"priority":10,
         "frameStart":1,"frameEnd":1,"damage0":0},
        {"name":"DEATH","anim":"STEP","type":17,"priority":100,"hold":2,
         "frameStart":1,"frameEnd":1,"damage0":1}],
      "damages":[{"type":3,"maxDistance":10,"damage":10},
        {"type":9,"minSpeed":10}]})");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    Combatant& critters = fixture.actor;
    fixture.open(device, root, nullptr, {}, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    REQUIRE(critters.present());
    const std::vector<EnemyView> party{targetAt(5)};
    fixture.update(6, 0.1f, party);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.takeBlows().size() == 1);
    EnemyHit hit;
    hit.damage = 1000;
    critters.hurt(hit);
    fixture.update(6, 0.1f, party);
    REQUIRE(critters.takeSpews().size() == 1);
    for (s32 frame = 0; frame < 15; ++frame) {
        fixture.update(6, 0.1f, party);
    }
    REQUIRE(critters.present());
    REQUIRE_FALSE(critters.moveDone());
    for (s32 frame = 0; frame < 6; ++frame) {
        fixture.update(6, 0.1f, party);
    }
    REQUIRE(!critters.present());
}
} // namespace
