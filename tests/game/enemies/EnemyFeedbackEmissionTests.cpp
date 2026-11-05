#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/audio/LevelSoundscape.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyFeedbackEmission.h"
#include "game/enemies/EnemyMissileEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path archiveFixture(std::string_view name,
                                     std::span<const std::string_view> trees) {
    const auto directory = test::scratchDirectory(name);
    std::filesystem::create_directories(directory);
    writeTextFile(directory / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    std::string text = R"({"trees":[)";
    for (const auto tree : trees) {
        if (text.back() != '[') {
            text += ',';
        }
        text += std::string{R"({"name":")"} + std::string{tree} + R"(",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"ACTIVE","frames":9,"rate":30}]})";
    }
    text += "]}";
    writeTextFile(directory / "animations.json", text);
    test::convertModelFixture(directory);
    return directory;
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive enemy;
    ItemArchive weapons;
    LevelInfo level;

    Fixture() {
        constexpr std::array<std::string_view, 3> kLocal{"BLOODFX1", "TREEHIT", "TREEDIE"};
        constexpr std::array<std::string_view, 6> kCommon{"BLOODFX1", "BLOODFX2", "HITCOL",
                                                          "HITDIE",   "TREEHIT",  "TREEDIE"};
        REQUIRE(enemy.load(archiveFixture("enemy-feedback-local", kLocal)));
        REQUIRE(weapons.load(archiveFixture("enemy-feedback-common", kCommon)));
        level.enemies = {LevelEnemy{13, 2, "EGRUNT"}, LevelEnemy{11, 1, "TRE"}};
        level.bossType = -1;
    }
};

TEST_CASE("swarm feedback resolves native archive priority opacity and sound independently",
          "[enemy-feedback-emission]") {
    Fixture fixture;
    EnemyFeedback hit{
        .kind = 13, .tier = 2, .close = true, .position = {2, 3, 4}, .yaw = 0.75f, .halfHeight = 4};
    auto emission =
        EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2HIT1CL");
    CHECK(emission.archive == &fixture.enemy);
    CHECK(emission.tree == "BLOODFX1");
    CHECK(emission.position == hit.position);
    CHECK(emission.setting.scale == 2);
    CHECK(emission.setting.yaw == hit.yaw);
    CHECK(emission.setting.tint.a == 96);

    hit.killed = true;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2DIECLO");
    CHECK(emission.archive == &fixture.weapons);
    CHECK(emission.tree == "BLOODFX2");
    hit.flags = damage::kNoHitEffect;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.sound == "S_EGRUNT2DIECLO");
    CHECK(emission.archive == nullptr);
    CHECK(emission.tree.empty());

    hit.flags = 0;
    hit.kind = 11;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.archive == &fixture.enemy);
    CHECK(emission.tree == "TREEDIE");
    CHECK(emission.setting.tint.a == 255);
    hit.killed = false;
    emission = EnemyFeedbackEmission::resolve(hit, nullptr, fixture.weapons, nullptr);
    CHECK(emission.archive == &fixture.weapons);
    CHECK(emission.tree == "TREEHIT");
    CHECK(emission.setting.tint.a == 255);
    CHECK(emission.sound.empty());
    hit.kind = 5;
    emission = EnemyFeedbackEmission::resolve(hit, nullptr, fixture.weapons, nullptr);
    CHECK(emission.tree == "HITCOL");
    CHECK(emission.setting.scale == 1);
    CHECK(emission.setting.tint.a == 96);

    hit.kind = 13;
    hit.flags = damage::kFire;
    emission = EnemyFeedbackEmission::resolve(hit, &fixture.enemy, fixture.weapons, &fixture.level);
    CHECK(emission.archive == nullptr); // neither native bank supplies FIREHIT
    CHECK(emission.tree.empty());
    CHECK(emission.sound == "S_EGRUNT2HIT1CL");
}

TEST_CASE("native missile impacts retain artwork opacity and splash volume",
          "[enemy-missile-emission]") {
    Fixture fixture;
    EnemyMissileHit hit;
    hit.position = {2, 3, 4};
    hit.worldContact = true;
    hit.flags = 2;
    auto emission = EnemyMissileEmission::resolve(hit, fixture.weapons);
    CHECK(emission.archive == &fixture.weapons);
    CHECK(emission.tree == "HITCOL");
    CHECK(emission.position == hit.position);
    CHECK(emission.setting.unlit);
    CHECK_FALSE(emission.setting.depthWrite);
    CHECK(emission.setting.tint.a == 96);
    CHECK(emission.sound.empty());
    CHECK(emission.level == 224);

    hit.liquid = true;
    emission = EnemyMissileEmission::resolve(hit, fixture.weapons);
    CHECK(emission.sound == "S_SPLASH");
    CHECK(emission.level == LevelSoundscape::kSplashLevel);
    hit.liquid = false;
    hit.burstRadius = 3;
    emission = EnemyMissileEmission::resolve(hit, fixture.weapons);
    CHECK(emission.archive == nullptr); // This fixture deliberately lacks EXPSMALL.
    CHECK(emission.tree.empty());
    CHECK(emission.setting.tint.a == 255);
    CHECK(emission.sound == "S_LOBBER_BOMB");
    CHECK(fixture.device.texturesCreated == 0); // Resolution never uploads or starts VFX.
}

TEST_CASE("native ricochets retain their sound without adding landing artwork",
          "[enemy-missile-emission]") {
    ItemArchive weapons;
    EnemyMissileHit hit;
    hit.position = {2, 3, 4};
    hit.ricochet = true;
    hit.liquid = true;
    hit.burstRadius = 3;
    const auto emission = EnemyMissileEmission::resolve(hit, weapons);
    CHECK(emission.position == hit.position);
    CHECK(emission.sound == "S_RICOCHET");
    CHECK(emission.level == 127);
    CHECK(emission.archive == nullptr);
    CHECK(emission.tree.empty());
}

TEST_CASE("enemy birth artwork obeys the first provider kind prefix and native tier fallback",
          "[enemy-feedback-emission][enemy-birth-emission]") {
    ItemArchive first;
    ItemArchive later;
    constexpr std::array<std::string_view, 2> kFirstTrees{"GENFX1", "GENFX3"};
    constexpr std::array<std::string_view, 3> kLaterTrees{"GENFX1", "GENFX2", "GENFX3"};
    REQUIRE(first.load(archiveFixture("enemy-birth-first", kFirstTrees)));
    REQUIRE(later.load(archiveFixture("enemy-birth-later", kLaterTrees)));
    std::array<ItemArchive*, kSwarmKindCount> archives{};
    archives[kGruntKind] = &first;
    archives[7] = &later;
    EnemyBirth birth{{3, 2, 1}, kGruntKind, 2};
    auto result = EnemyBirthEmission::resolve(birth, archives);
    CHECK(result.archive == &first);
    CHECK(result.tree == "GENFX1"); // no tier-two tree in the first provider
    CHECK(result.position == birth.position);
    birth.tier = 3;
    result = EnemyBirthEmission::resolve(birth, archives);
    CHECK(result.tree == "GENFX3");
    birth.kind = 1; // an earlier tall kind borrows the first provider
    CHECK(EnemyBirthEmission::resolve(birth, archives).archive == &first);
    for (const s32 kind : {kRatKind, 7, -1, kSwarmKindCount}) {
        birth.kind = kind;
        result = EnemyBirthEmission::resolve(birth, archives);
        CHECK(result.archive == nullptr);
        CHECK(result.tree.empty());
    }
    archives[kGruntKind] = nullptr;
    birth.kind = 7;
    birth.tier = 2;
    result = EnemyBirthEmission::resolve(birth, archives);
    CHECK(result.archive == &later);
    CHECK(result.tree == "GENFX2");
}

} // namespace
