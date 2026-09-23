#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CritterData.h"
#include "game/enemies/Critters.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path attackTable(std::string_view table) {
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
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"DJINN",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
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

TEST_CASE("boss health gates use exclusive upper bounds only above the lower bound",
          "[game][boss-attacks]") {
    CritterTarget target;
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

TEST_CASE("boss attack rotation does not starve equal-priority attacks", "[game][boss-attacks]") {
    const auto root = attackTable(R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":4,"maxHealth":100}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"A","anim":"STEP","type":128,"priority":10},
        {"name":"B","anim":"STEP","type":128,"priority":10},
        {"name":"LINK_ONLY","anim":"STEP","type":128,"priority":100,"flags":4}]})");
    test::FakeRenderDevice device;
    Critters critters;
    critters.open(device, root, nullptr, {}, 'C');
    const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, "DJINN");
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{targetAt(8)};
    critters.update(6, 0.1f, party);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "A");
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "B");
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "A");
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
    Critters critters;
    critters.open(device, root, nullptr, {}, 'C');
    const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, "DJINN");
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{targetAt(8)};
    critters.update(6, 0.1f, party);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "A");
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "B");
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "B");
    critters.update(6, 0.1f, party);
    // A pattern has its own timestamp; it does not consume its members' solo cooldowns.
    REQUIRE(critters.moveOf(*id) == "A");
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
    Critters critters;
    critters.open(device, root, nullptr, {}, 'C');
    const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, "DJINN");
    REQUIRE(id.has_value());
    std::vector<EnemyView> party{targetAt(5), targetAt(20)};
    party[1].player = 1;
    critters.update(6, 0.1f, party);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "HEALTHY");
    EnemyHit hit;
    hit.damage = 50;
    critters.hurt(*id, hit);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.moveOf(*id) == "WOUNDED");
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
    Critters critters;
    critters.open(device, root, nullptr, {}, 'C');
    const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, "DJINN");
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{targetAt(5)};
    critters.update(6, 0.1f, party);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.takeBlows().size() == 1);
    EnemyHit hit;
    hit.damage = 1000;
    critters.hurt(*id, hit);
    critters.update(6, 0.1f, party);
    REQUIRE(critters.takeSpews().size() == 1);
    for (s32 frame = 0; frame < 15; ++frame) {
        critters.update(6, 0.1f, party);
    }
    REQUIRE(critters.count() == 1);
    REQUIRE_FALSE(critters.moveDoneOf(*id));
    for (s32 frame = 0; frame < 6; ++frame) {
        critters.update(6, 0.1f, party);
    }
    REQUIRE(critters.count() == 0);
}
} // namespace
