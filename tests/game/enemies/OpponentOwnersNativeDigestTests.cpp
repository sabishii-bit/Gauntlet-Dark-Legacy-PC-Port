#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/bosses/Bosses.h"
#include "game/critters/Critters.h"
#include "game/critters/Gargoyle.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/level/BossKinds.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class Owner> struct NativeOwner {
    test::FakeRenderDevice device;
    NativeStateResources resources;
    Owner owner;

    NativeOwner() {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        owner.open(device, root, nullptr, {}, 'G');
    }
    NativeStateDigest::Digest digest() {
        REQUIRE(owner.registerNativeResources(resources, "stage/G5/test-owner"));
        NativeStateDigest value{resources.resources()};
        REQUIRE(owner.appendNativeDigest(value));
        REQUIRE(value.checkedFinish());
        return *value.checkedFinish();
    }
    NativeStateDigest::Digest digest(const typename Owner::LocalSnapshot& saved) {
        REQUIRE(owner.registerNativeResources(resources, "stage/G5/test-owner"));
        NativeStateDigest value{resources.resources()};
        REQUIRE(owner.appendLocalDigest(value, saved));
        REQUIRE(value.checkedFinish());
        return *value.checkedFinish();
    }
};

/** A golem's, a general's and an eagle gargoyle's data and artwork of their own: each body
 * stands, walks, strikes within five, is knocked down and dies. */
std::filesystem::path familyStock() {
    const auto root = test::scratchDirectory("opponent-owners-families");
    std::filesystem::create_directories(root / "critter");
    for (const auto& definition :
         {Golem::definition(), General::definition(), Gargoyle::definition()}) {
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
        writeTextFile(root / "critter" / (definition.name + ".json"),
                      R"({"descriptors":[{"prefix":"BODY","name":")" + definition.name +
                          R"(","type":)" + std::to_string(static_cast<s32>(definition.kind)) +
                          R"(}],
          "types":[{"moveCount":5,"maxHealth":100,"radius":1,"expValue":50}],
          "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
                   {"name":"WALK","anim":"STEP","type":52,"priority":10,"speed":3},
                   {"name":"ATTACK","anim":"STEP","type":128,"priority":20,
                    "target":{"maxDistance":5}},
                   {"name":"KD","anim":"STEP","type":66,"priority":100},
                   {"name":"DEATH","anim":"STEP","type":17,"priority":999}]})");
    }
    return root;
}

TEST_CASE("native boss digest observes hidden waking and legend clocks",
          "[native-digest][opponent-owners-native-digest]") {
    Bosses boss;
    const Bosses foreign{};
    Bosses::LocalSnapshot saved;
    REQUIRE(boss.saveLocal(saved));
    NativeStateDigest initial;
    NativeStateDigest historical;
    NativeStateDigest changed;
    REQUIRE(boss.appendNativeDigest(initial));
    boss.arm(1.0f / 60);
    REQUIRE_FALSE(boss.present());
    REQUIRE(boss.appendNativeDigest(changed));
    REQUIRE(boss.appendLocalDigest(historical, saved));
    CHECK(initial.checkedFinish() == historical.checkedFinish());
    CHECK(initial.checkedFinish() != changed.checkedFinish());
    NativeStateDigest rejected;
    CHECK_FALSE(foreign.appendLocalDigest(rejected, saved));
    CHECK_FALSE(rejected.checkedFinish());
    boss.close();
    NativeStateDigest stale;
    CHECK_FALSE(boss.appendLocalDigest(stale, saved));

    LegendRite rite;
    rite.begin(1, *legendWeaknessOf(38));
    rite.update(1, true, false);
    const auto before = rite;
    const auto phase = rite.stage();
    REQUIRE(rite.update(1, true, false).empty());
    REQUIRE(rite.stage() == phase);
    NativeStateDigest beforeHash;
    NativeStateDigest afterHash;
    beforeHash.value(before);
    afterHash.value(rite);
    CHECK(beforeHash.checkedFinish() != afterHash.checkedFinish());
}

TEST_CASE("native critter owner hashes hidden routes without reading a saved patrol's live route",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto population = std::make_unique<NativeOwner<Critters>>();
    population->owner.setLookouts({{{0, 0, 0}, {0, 0, 20}}, {1, 65535}});
    REQUIRE(population->owner.spawnGeneral({}, 0, 15));
    Critters::LocalSnapshot saved;
    REQUIRE(population->owner.saveLocal(saved));
    const auto initial = population->digest();
    REQUIRE(population->digest(saved) == initial);
    population->owner.setLookouts({{{2, 0, 0}, {2, 0, 20}}, {1, 65535}});
    CHECK(population->digest() != initial);
    CHECK(population->digest(saved) == initial);
    REQUIRE(population->owner.restoreLocal(saved));
    CHECK(population->digest() == initial);
}

TEST_CASE("native critter owner includes discarded species histories and native texture clocks",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto first = std::make_unique<NativeOwner<Critters>>();
    auto second = std::make_unique<NativeOwner<Critters>>();
    Critters::LocalSnapshot cold;
    Critters::LocalSnapshot general;
    Critters::LocalSnapshot speculative;
    REQUIRE(first->owner.saveLocal(cold));
    const auto empty = first->digest();
    REQUIRE(first->owner.spawnGeneral({}, 0, 15));
    REQUIRE(second->owner.spawnGeneral({}, 0, 15));
    REQUIRE(first->owner.saveLocal(general));
    const auto initial = first->digest();
    CHECK(initial == second->digest());
    REQUIRE(first->owner.spawnGolem({4, 0, 3}, 0));
    REQUIRE(first->owner.spawnGargoyle({8, 0, 3}, 0));
    REQUIRE(first->owner.saveLocal(speculative));
    // No resource registration in the speculative branch. The cache, not the
    // rolled-back logical roster, must register these saved borrowed bindings.
    REQUIRE(first->owner.restoreLocal(general));
    const auto uploads = first->device.texturesCreated;
    CHECK(first->digest() == initial);
    CHECK(first->digest(speculative) != initial);
    CHECK(first->device.texturesCreated == uploads);
    first->owner.resources().front()->textures.advance(1.0f / 60);
    CHECK(first->digest() != initial);
    CHECK(first->digest(general) == initial);
    REQUIRE(first->owner.restoreLocal(general));
    CHECK(first->digest() == second->digest());
    REQUIRE(first->owner.restoreLocal(cold));
    CHECK(first->digest() == empty);
    CHECK(first->device.textureUpdates == 0);
    NativeStateDigest foreign{second->resources.resources()};
    CHECK_FALSE(second->owner.appendLocalDigest(foreign, general));
    first->owner.close();
    NativeStateDigest stale{first->resources.resources()};
    CHECK_FALSE(first->owner.appendLocalDigest(stale, general));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("native boss owner hashes all families and recursive children across independent loads",
          "[native-digest][opponent-owners-native-digest][assets]") {
    const s32 kind = GENERATE(34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44);
    CAPTURE(kind);
    auto first = std::make_unique<NativeOwner<Bosses>>();
    auto second = std::make_unique<NativeOwner<Bosses>>();
    Bosses::LocalSnapshot cold;
    Bosses::LocalSnapshot saved;
    REQUIRE(first->owner.saveLocal(cold));
    const auto empty = first->digest();
    REQUIRE(first->owner.spawn(kind, {}, 0, 30));
    REQUIRE(second->owner.spawn(kind, {}, 0, 30));
    std::array<EnemyView, 1> party;
    party[0].player = 0;
    party[0].position = {0, 0, 12};
    party[0].height = 6;
    party[0].radius = 1;
    for (s32 tick = 0; tick < 160; ++tick) {
        first->owner.update(1, 1.0f / 60, party);
        second->owner.update(1, 1.0f / 60, party);
    }
    REQUIRE(first->owner.saveLocal(saved));
    const auto current = first->digest();
    CHECK(current == second->digest());
    CHECK(first->digest(saved) == current);
    const auto uploads = first->device.texturesCreated;
    first->owner.draw(first->device, Mat4{1}, {}, nullptr, 0.25f);
    first->owner.draw(first->device, Mat4{1}, {}, nullptr, 0.75f);
    CHECK(first->digest() == current);
    REQUIRE(first->owner.restoreLocal(cold));
    CHECK(first->digest() == empty);
    CHECK(first->digest(saved) == current);
    REQUIRE(first->owner.restoreLocal(saved));
    CHECK(first->digest() == second->digest());
    CHECK(first->device.texturesCreated == uploads);
    CHECK(first->device.textureUpdates == 0);
}

TEST_CASE("native combatant owner fails closed without family registration",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto population = std::make_unique<NativeOwner<Critters>>();
    REQUIRE(population->owner.spawnGeneral({}, 0, 15));
    NativeStateDigest missing;
    CHECK_FALSE(population->owner.appendNativeDigest(missing));
    CHECK_FALSE(missing.checkedFinish());
    CHECK_FALSE(
        population->resources.registerCombatantAssets(*population->owner.resources().front(), ""));
}

TEST_CASE("empty native opponent owners include configuration for their next birth",
          "[native-digest][opponent-owners-native-digest]") {
    test::FakeRenderDevice device;
    Bosses boss;
    Critters critters;
    NativeStateResources resources;
    const WorldCollision collision;
    const HazardSurfaces hazards;
    REQUIRE(resources.registerCollision(collision, "stage/test"));
    REQUIRE(resources.registerHazards(hazards, "stage/test"));
    const auto bossHash = [&] {
        NativeStateDigest digest{resources.resources()};
        REQUIRE(boss.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    const auto critterHash = [&] {
        NativeStateDigest digest{resources.resources()};
        REQUIRE(critters.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    // No actors or archives are loaded in these configurations.
    boss.open(device, {}, nullptr, {}, 'G');
    critters.open(device, {}, nullptr, {}, 'G');
    const auto bossInitial = bossHash();
    const auto crittersInitial = critterHash();
    EnemyScales scaled;
    scaled.speed = 2;
    boss.open(device, {}, nullptr, scaled, 'G');
    critters.open(device, {}, nullptr, scaled, 'G');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    boss.open(device, {}, nullptr, {}, 'A');
    critters.open(device, {}, nullptr, {}, 'A');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    boss.open(device, {}, &collision, {}, 'G');
    critters.open(device, {}, &collision, {}, 'G');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    critters.open(device, {}, nullptr, {}, 'G', {}, "EAGL");
    CHECK(critterHash() != crittersInitial);
    critters.open(device, {}, nullptr, {}, 'G');
    CHECK(critterHash() == crittersInitial);
    Critters::LocalSnapshot beforeHazards;
    REQUIRE(critters.saveLocal(beforeHazards));
    critters.setHazards(&hazards);
    CHECK(critterHash() != crittersInitial);
    NativeStateDigest refused{resources.resources()};
    CHECK_FALSE(critters.appendLocalDigest(refused, beforeHazards));
    CHECK_FALSE(refused.checkedFinish());
}

TEST_CASE("empty native opponent owner digest bytes stay pinned for distinct configurations",
          "[native-digest][opponent-owners-native-digest][native-digest-pin]") {
    test::FakeRenderDevice device;
    Bosses boss;
    Critters critters;
    NativeStateResources resources;
    const WorldCollision collision;
    const HazardSurfaces hazards;
    REQUIRE(resources.registerCollision(collision, "stage/test"));
    REQUIRE(resources.registerHazards(hazards, "stage/test"));
    EnemyScales scales;
    scales.health = 1.25f;
    scales.speed = 2;
    scales.sight = 0.75f;
    scales.damage = 1.5f;
    scales.missileRate = 0.5f;
    scales.missileAim = 0.25f;
    scales.playerLevel = 12;
    scales.bossEncounter = true;
    scales.players = 3;
    boss.open(device, {}, &collision, scales, 'A');
    critters.open(device, {}, &collision, scales, 'A', {}, "EAGL");
    critters.setHazards(&hazards);
    NativeStateDigest bossDigest{resources.resources()};
    NativeStateDigest critterDigest{resources.resources()};
    REQUIRE(boss.appendNativeDigest(bossDigest));
    REQUIRE(critters.appendNativeDigest(critterDigest));
    REQUIRE(bossDigest.checkedFinish());
    REQUIRE(critterDigest.checkedFinish());
    // SHA-256 of the native digest of each owner above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kBoss =
        "24411247ba67fb12f858251bc3b13690e3c32f464915852b835dc72f1e59cc26";
    constexpr std::string_view kCritters =
        "29afe57e7fec8e97be224e179920192fbb1323d9abad55904433096c27fc6241";
    CHECK(test::hexBytes(*bossDigest.checkedFinish()) == kBoss);
    CHECK(test::hexBytes(*critterDigest.checkedFinish()) == kCritters);
}

TEST_CASE("native critter owner digest bytes stay pinned after three families' script and its save",
          "[native-digest][opponent-owners-native-digest][digest-golden]") {
    test::FakeRenderDevice device;
    Critters critters;
    critters.open(device, familyStock(), nullptr, {}, 'G');
    critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
    REQUIRE(critters.spawnGeneral({-20, 0, 0}, 0, 20) == 0);
    REQUIRE(critters.spawnGolem({0, 0, 0}, 0) == 1);
    REQUIRE(critters.spawnGargoyle({20, 0, 0}, 0) == 2);
    std::array<EnemyView, 2> party;
    party[0].player = 3;
    party[0].position = {0, 0, 4};
    party[1].player = 1;
    party[1].position = {20, 0, 6};
    for (EnemyView& player : party) {
        player.radius = 1;
        player.height = 6;
    }
    constexpr f32 kStep = 1.0f / 60;
    for (s32 frame = 0; frame < 10; ++frame) {
        critters.update(1, kStep, party);
    }
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 11;
    hit.close = true;
    critters.hurt(1, hit);
    Critters::LocalSnapshot saved;
    REQUIRE(critters.saveLocal(saved));
    critters.freeze(0, 5);
    critters.blind(2, 7);
    for (s32 frame = 10; frame < 22; ++frame) {
        critters.update(1, kStep, party);
    }
    NativeStateResources resources;
    REQUIRE(critters.registerNativeResources(resources, "stage/opponent-owners/critters"));
    // SHA-256 of the population's native digest 22 frames in and of what it saved ten frames in,
    // after a blow on the golem and before the general was frozen and the gargoyle blinded, which
    // netplay peers must hash alike: a literal moves only when what is visited changes on purpose.
    test::checkPinnedDigests("Critters", test::liveDigest(critters, resources.resources()),
                             test::savedDigest(critters, saved, resources.resources()),
                             "29db3f18e5c05bfbda387b254dbed9b2d197148c11808f082b9dacb04e697fcc",
                             "673bc634f7184537d599e3a96636e923b75b1ed40600964f72623aa3f4e6a962");
}

TEST_CASE("native boss owner digest bytes stay pinned after a headed boss wakes and its save",
          "[native-digest][opponent-owners-native-digest][digest-golden]") {
    test::FakeRenderDevice device;
    Bosses boss;
    boss.open(device, test::headedBodyAssets(), nullptr, {}, 'A');
    REQUIRE(boss.spawn(boss_type::kChimera, {}, 0, 30));
    std::array<EnemyView, 1> party;
    party[0].player = 0;
    party[0].position = {0, 0, 12};
    party[0].height = 6;
    party[0].radius = 1;
    constexpr f32 kStep = 1.0f / 60;
    for (s32 tick = 0; tick < 60; ++tick) {
        boss.update(1, kStep, party);
    }
    REQUIRE_FALSE(boss.view().awake);
    Bosses::LocalSnapshot saved;
    REQUIRE(boss.saveLocal(saved));
    for (s32 tick = 60; tick < 180; ++tick) {
        boss.update(1, kStep, party);
    }
    REQUIRE(boss.view().awake);
    NativeStateResources resources;
    REQUIRE(boss.registerNativeResources(resources, "stage/opponent-owners/bosses"));
    // SHA-256 of the boss owner's native digest three seconds in, awake, and of what it saved a
    // second in, still waiting, which netplay peers must hash alike: a literal moves only when
    // what is visited changes on purpose.
    test::checkPinnedDigests("Bosses", test::liveDigest(boss, resources.resources()),
                             test::savedDigest(boss, saved, resources.resources()),
                             "fad7daca4d25a7fab78240d412d963b9fd04e09736743560c477d476e5c54527",
                             "c204b97f64e450055fc0886b4d4e6e3fefe4c838835a29d22081b197dd011269");
}
} // namespace
