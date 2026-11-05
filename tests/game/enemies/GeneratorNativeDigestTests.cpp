#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Generators.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;
constexpr std::string_view kSwarmPrefix = "stage/generator-fixture/swarm";

std::filesystem::path generatorDigestFixture() {
    const auto root = test::scratchDirectory("generator-native-digest");
    const auto archive = root / "MONSTERS/GRU";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 1\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"name":"BODY","file":"body.obj"},
      {"name":"GEN_GRU0","file":"body.obj"},
      {"name":"GEN_GRU1","file":"body.obj"},
      {"name":"GEN_GRU2","file":"body.obj"},
      {"name":"GEN_GRU3","file":"body.obj"}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"name":"SKIN","file":"skin.png","width":2,"height":2},
      {"name":"GLOW","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({
      "textureAnimations":[{"name":"SKIN","texture":0,"source":0,"frames":2,"rate":3}],
      "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[
        {"name":"GRU1","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
         "sequences":[{"name":"READY","frames":10,"frameRate":30},
                      {"name":"WALK","frames":10,"frameRate":30}]},
        {"name":"GRU2","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
         "sequences":[{"name":"READY","frames":10,"frameRate":30},
                      {"name":"WALK","frames":10,"frameRate":30}]},
        {"name":"GRU3","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
         "sequences":[{"name":"READY","frames":10,"frameRate":30},
                      {"name":"WALK","frames":10,"frameRate":30}]},
        {"name":"BOSSGEN","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
          {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
         "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
           "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]}]})");
    // A second species is admitted only when the speculative boss generator is
    // born. Its unique tree forces the figure to borrow this archive, not GRU.
    const auto rat = root / "MONSTERS/RAT";
    std::filesystem::create_directories(rat);
    writeTextFile(rat / "body.obj",
                  "v 0 0 1\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(rat / "objects.json", R"({"objects":[
      {"name":"BODY","file":"body.obj"}]})");
    writeFile(rat / "skin.png", test::kTinyPng);
    writeTextFile(rat / "textures.json", R"({"bitmaps":[
      {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(rat);
    writeTextFile(rat / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[
        {"name":"RAT1","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
         "sequences":[{"name":"READY","frames":10,"frameRate":30}]},
        {"name":"RAT_ARROW","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}]},
        {"name":"RAT_BOMB","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}]},
        {"name":"RAT_FBALL","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}]},
        {"name":"RATGEN","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
          {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
         "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30}]}]})");
    writeTextFile(root / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                     "collisionType":1,"hitPoints":10}],
      "itemInstances":[
        {"info":0,"minPlayers":1,"position":[0,0,0],
         "params":[2,0,7,0,2,0,1,0,0,0,0,0]},
        {"info":0,"minPlayers":2,"position":[30,0,0],
         "params":[3,0,7,0,2,0,1,0,0,0,0,0]}]})");
    return root;
}

struct Pool {
    test::FakeRenderDevice device;
    WorldCollision collision;
    WorldLayout layout;
    Enemies enemies;
    Generators generators;
    NativeStateResources resources;
    std::array<EnemyView, 1> players{EnemyView{.player = 0, .position = {0, 0, 25}}};

    explicit Pool(const std::filesystem::path& path) {
        REQUIRE(layout.load(path));
        CollisionTriangle lower;
        lower.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, -200}, Vec3{200, 0, 200}};
        lower.normal = {0, 1, 0};
        lower.object = 0;
        CollisionTriangle upper = lower;
        upper.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{-200, 0, 200}};
        collision.build({lower, upper});
        enemies.open(device, path, &collision, 8, {}, 347);
        REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
        REQUIRE(generators.count() == 2);
        REQUIRE(generators.bodyResources().size() == 4);
        REQUIRE(enemies.registerNativeResources(resources, kSwarmPrefix));
        REQUIRE(resources.registerCollision(collision, "generator-fixture/world"));
        REQUIRE(generators.registerNativeResources(resources));
    }

    NativeStateDigest::Digest digest() const {
        NativeStateDigest value(resources.resources());
        REQUIRE(generators.appendNativeDigest(value));
        const auto result = value.checkedFinish();
        REQUIRE(result);
        return *result;
    }

    NativeStateDigest::Digest digest(const Generators::LocalSnapshot& saved) const {
        NativeStateDigest value(resources.resources());
        REQUIRE(generators.appendLocalDigest(value, saved));
        const auto result = value.checkedFinish();
        REQUIRE(result);
        return *result;
    }

    void boss(bool settled = false) {
        ItemInfo info;
        info.type = ItemInfo::kGenerator;
        info.name = "BOSSGEN";
        info.hitPoints = 20;
        info.radius = 2;
        info.height = 5;
        REQUIRE(generators.placeBoss(device, info, *enemies.archive(kGruntKind), enemies,
                                     kGruntKind, glm::translate(Mat4{1}, Vec3{60, 0, 0}),
                                     &collision, "BOSSGEN", settled));
        REQUIRE(resources.refreshArchive(*enemies.archiveOf(kGruntKind)));
        REQUIRE(generators.registerNativeResources(resources));
    }

    void step(s32 tick, bool wrong = false) {
        players[0].position.x = wrong ? -80.0f : static_cast<f32>(tick % 13);
        generators.setPlayerCount(wrong ? 4 : 1 + (tick / 6) % 2);
        generators.setView(std::nullopt, wrong ? std::optional<Vec3>{{10000, 0, 0}} : std::nullopt);
        if (tick % 7 == 2) {
            (void)generators.strike(0, wrong ? 1000.0f : 1.0f, 0);
        }
        if (tick % 9 == 5 && enemies.count() > 0) {
            EnemyHit hit;
            hit.player = 0;
            hit.damage = wrong ? 1.0f : 1000.0f;
            (void)enemies.hurt(0, hit);
        }
        const bool stopped = wrong || tick % 17 < 3;
        generators.update(1, enemies, players, {}, stopped);
        enemies.update(1, kStep, players, generators.enemyObstacles(), nullptr, 1, stopped);
    }
};

TEST_CASE("generator native hashes use semantic art bindings across separate loads",
          "[native-state-digest][generator-native-digest]") {
    const auto root = generatorDigestFixture();
    Pool first(root);
    Pool second(root);
    REQUIRE(first.enemies.archiveOf(kGruntKind) != second.enemies.archiveOf(kGruntKind));
    REQUIRE(first.generators.bodyResources()[0].tree != second.generators.bodyResources()[0].tree);
    CHECK(first.digest() == second.digest());
    Generators::LocalSnapshot initial;
    first.generators.saveLocal(initial);
    CHECK(first.digest(initial) == first.digest());

    // A player-count change is not yet visible, but changes next-tick presence.
    const auto before = first.digest();
    first.generators.setPlayerCount(2);
    CHECK(first.generators.presentation().size() == second.generators.presentation().size());
    CHECK(first.digest() != before);
    REQUIRE(first.generators.restoreLocal(initial));
    CHECK(first.digest() == before);

    ViewVolume view;
    first.generators.setView(view, Vec3{1, 0, 0});
    CHECK(first.digest() != before);
    REQUIRE(first.generators.restoreLocal(initial));
    first.generators.update(1, first.enemies, first.players, {}, true);
    CHECK(first.digest() != before);
    // Saved body clocks must not read through to the bank's later live clock.
    CHECK(first.digest(initial) == before);
    REQUIRE(first.generators.restoreLocal(initial));
    CHECK(first.digest() == second.digest());

    first.boss();
    second.boss();
    const auto uploads = first.device.texturesCreated;
    CHECK(first.digest() == second.digest());
    for (s32 tick = 0; tick < 32; ++tick) {
        CAPTURE(tick);
        first.step(tick);
        second.step(tick);
        REQUIRE(first.digest() == second.digest());
        const auto native = first.digest();
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            first.generators.draw(first.device, Mat4{1}, WorldLighting{}, alpha);
            CHECK(first.digest() == native);
        }
    }
    CHECK(first.device.texturesCreated == uploads);
    CHECK(first.device.textureUpdates == 0);
}

TEST_CASE("generator native hashes retain suppressed and destroyed bodies",
          "[native-state-digest][generator-native-digest]") {
    const auto root = generatorDigestFixture();
    Pool first(root);
    Pool second(root);
    ViewVolume offscreen;
    offscreen.position = {10000, 0, 0};
    first.generators.setView(offscreen);
    second.generators.setView(offscreen);
    first.generators.setPlayerCount(2);
    second.generators.setPlayerCount(2);
    first.generators.update(1, first.enemies, first.players, {}, true);
    second.generators.update(1, second.enemies, second.players, {}, true);
    REQUIRE(first.generators.strike(1, 1, 0));
    first.generators.setPlayerCount(1);
    second.generators.setPlayerCount(1);
    first.generators.update(1, first.enemies, first.players, {}, true);
    second.generators.update(1, second.enemies, second.players, {}, true);
    first.generators.setView(std::nullopt);
    second.generators.setView(std::nullopt);
    first.generators.update(1, first.enemies, first.players, {}, true);
    second.generators.update(1, second.enemies, second.players, {}, true);
    REQUIRE(first.generators.slot(1).presence == Generators::Slot::Presence::Suppressed);
    CHECK(first.generators.presentation().size() == second.generators.presentation().size());
    CHECK(first.digest() != second.digest());

    // A destroyed boss retains its animation/emitter state even after it stops
    // drawing. Different settled/landing state must not disappear from the hash.
    Pool landing(root);
    Pool settled(root);
    landing.boss(false);
    settled.boss(true);
    REQUIRE(landing.generators.strike(2, 1000, 0));
    REQUIRE(settled.generators.strike(2, 1000, 0));
    REQUIRE_FALSE(landing.generators.standing(2));
    REQUIRE_FALSE(settled.generators.standing(2));
    CHECK(landing.generators.slot(2) == settled.generators.slot(2));
    CHECK(landing.digest() != settled.digest());
}

TEST_CASE("rewound species retain hashable artwork without becoming active again",
          "[native-state-digest][generator-native-digest][local-rewind]") {
    const auto root = generatorDigestFixture();
    Pool corrected(root);
    const Pool uninterrupted(root);
    Generators::LocalSnapshot initialGenerators;
    Enemies::LocalSnapshot initialEnemies;
    corrected.generators.saveLocal(initialGenerators);
    REQUIRE(corrected.enemies.saveLocal(initialEnemies));
    REQUIRE_FALSE(corrected.enemies.kindLoaded(kRatKind));

    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 20;
    info.radius = 2;
    info.height = 5;
    REQUIRE(corrected.generators.placeBoss(
        corrected.device, info, *corrected.enemies.archive(kGruntKind), corrected.enemies, kRatKind,
        glm::translate(Mat4{1}, Vec3{60, 0, 0}), &corrected.collision, "RATGEN"));
    REQUIRE(corrected.generators.count() == 3);
    const auto* retainedArchive = corrected.enemies.archiveOf(kRatKind);
    REQUIRE(retainedArchive);
    REQUIRE(retainedArchive->trees.find("RATGEN"));
    const std::array retainedShots{corrected.enemies.projectileModel(kRatKind, 0),
                                   corrected.enemies.projectileModel(kRatKind, 1),
                                   corrected.enemies.projectileModel(kRatKind, 2)};
    for (const auto* model : retainedShots) {
        REQUIRE(model);
    }

    // No RAT resource registration or state hash occurs before this rewind.
    // Both native owners mutated by placeBoss return to their original values;
    // the artwork caches deliberately retain the speculative branch's bindings.
    REQUIRE(corrected.generators.restoreLocal(initialGenerators));
    REQUIRE(corrected.enemies.restoreLocal(initialEnemies));
    REQUIRE(corrected.enemies.archiveOf(kRatKind) == nullptr);
    REQUIRE_FALSE(corrected.enemies.kindLoaded(kRatKind));
    Generators::LocalSnapshot afterGenerators;
    Enemies::LocalSnapshot afterEnemies;
    corrected.generators.saveLocal(afterGenerators);
    REQUIRE(corrected.enemies.saveLocal(afterEnemies));
    CHECK(corrected.generators.equalLocal(initialGenerators, afterGenerators));
    CHECK(corrected.enemies.equalLocal(initialEnemies, afterEnemies));

    // The old active-species-only registration misses the retained prototype.
    NativeStateResources activeOnly;
    REQUIRE(activeOnly.registerArchive(*corrected.enemies.archiveOf(kGruntKind),
                                       "stage/generator-fixture/swarm/4"));
    REQUIRE(activeOnly.registerCollision(corrected.collision, "generator-fixture/world"));
    CHECK_FALSE(corrected.generators.registerNativeResources(activeOnly));
    CHECK_FALSE(activeOnly.good());

    const auto uploads = corrected.device.texturesCreated;
    REQUIRE(corrected.enemies.registerNativeResources(corrected.resources, kSwarmPrefix));
    REQUIRE(corrected.generators.registerNativeResources(corrected.resources));
    CHECK(corrected.digest() == uninterrupted.digest());
    CHECK(corrected.digest(initialGenerators) == uninterrupted.digest());
    NativeStateDigest retained(corrected.resources.resources());
    retained.resource(retainedArchive);
    for (const auto* model : retainedShots) {
        retained.resource(model);
    }
    CHECK(retained.checkedFinish());
    REQUIRE(corrected.enemies.saveLocal(afterEnemies));
    CHECK(corrected.enemies.equalLocal(initialEnemies, afterEnemies));
    CHECK(corrected.enemies.archiveOf(kRatKind) == nullptr);
    CHECK(corrected.device.texturesCreated == uploads);
    CHECK(corrected.device.textureUpdates == 0);
}

TEST_CASE("generator native hashes match corrected one to eight tick replays",
          "[native-state-digest][generator-native-digest][local-rewind]") {
    Pool pool(generatorDigestFixture());
    pool.boss();
    Generators::LocalSnapshot initial;
    Enemies::LocalSnapshot initialEnemies;
    pool.generators.saveLocal(initial);
    REQUIRE(pool.enemies.saveLocal(initialEnemies));
    const auto original = pool.digest();
    const auto uploads = pool.device.texturesCreated;
    bool sawBirth = false;
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(pool.generators.restoreLocal(initial));
        REQUIRE(pool.enemies.restoreLocal(initialEnemies));
        std::vector<NativeStateDigest::Digest> expected;
        std::vector<Generators::LocalSnapshot> expectedValues(static_cast<usize>(distance));
        for (s32 tick = 0; tick < distance; ++tick) {
            pool.step(tick + 3);
            expected.push_back(pool.digest());
            pool.generators.saveLocal(expectedValues[static_cast<usize>(tick)]);
            sawBirth = sawBirth || pool.generators.bredOf(0) > 0;
        }
        REQUIRE(pool.generators.restoreLocal(initial));
        REQUIRE(pool.enemies.restoreLocal(initialEnemies));
        for (s32 tick = 0; tick < distance; ++tick) {
            pool.step(tick + 3, true);
        }
        CHECK(pool.digest() != expected.back());
        CHECK(pool.digest(initial) == original);
        REQUIRE(pool.generators.restoreLocal(initial));
        REQUIRE(pool.enemies.restoreLocal(initialEnemies));
        for (s32 tick = 0; tick < distance; ++tick) {
            CAPTURE(tick);
            pool.step(tick + 3);
            CHECK(pool.digest() == expected[static_cast<usize>(tick)]);
            CHECK(pool.digest(expectedValues[static_cast<usize>(tick)]) == pool.digest());
            Generators::LocalSnapshot corrected;
            pool.generators.saveLocal(corrected);
            CHECK(pool.generators.equalLocal(corrected, expectedValues[static_cast<usize>(tick)]));
        }
    }
    CHECK(sawBirth);
    CHECK(pool.device.texturesCreated == uploads);
    CHECK(pool.device.textureUpdates == 0);
}

TEST_CASE("generator native digest refuses missing resources and foreign or stale history",
          "[native-state-digest][generator-native-digest]") {
    const auto root = generatorDigestFixture();
    Pool first(root);
    const Pool second(root);
    NativeStateDigest missing;
    CHECK_FALSE(first.generators.appendNativeDigest(missing));
    CHECK_FALSE(missing.checkedFinish());
    NativeStateResources incomplete;
    REQUIRE(incomplete.registerCollision(first.collision, "generator-fixture/world"));
    REQUIRE(incomplete.registerArchive(*first.enemies.archiveOf(kGruntKind), "MONSTERS/GRU"));
    NativeStateDigest unregisteredBodies(incomplete.resources());
    CHECK_FALSE(first.generators.appendNativeDigest(unregisteredBodies));
    CHECK_FALSE(unregisteredBodies.checkedFinish());

    Generators::LocalSnapshot saved;
    first.generators.saveLocal(saved);
    NativeStateDigest untouched(second.resources.resources());
    untouched.string("unchanged");
    const auto before = untouched.finish();
    CHECK_FALSE(second.generators.appendLocalDigest(untouched, saved));
    CHECK_FALSE(untouched.checkedFinish());
    CHECK(untouched.finish() == before);
    NativeStateDigest stale(first.resources.resources());
    stale.string("unchanged");
    first.generators.clear();
    CHECK_FALSE(first.generators.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
    CHECK(stale.finish() == before);
}

TEST_CASE("generator native digest bytes stay pinned for a bound field and a boss generator",
          "[native-state-digest][generator-native-digest][native-digest-pin]") {
    Pool pool(generatorDigestFixture());
    pool.boss(true);
    pool.generators.setPlayerCount(2);
    ViewVolume view;
    view.position = {5, 0, 10};
    pool.generators.setView(view, Vec3{1, 0, 0});
    // SHA-256 of the native digest of the generators above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "547218d5649920f32598dfda1f17a97c9046fda8559a6d6df374924ffa8ed82e";
    CHECK(test::hexBytes(pool.digest()) == kDigest);
}

TEST_CASE("generator native digest bytes stay pinned after the breeding script and what it saved",
          "[native-state-digest][generator-native-digest][digest-golden]") {
    Pool pool(generatorDigestFixture());
    pool.boss();
    for (s32 tick = 3; tick < 7; ++tick) {
        pool.step(tick);
    }
    Generators::LocalSnapshot saved;
    pool.generators.saveLocal(saved);
    for (s32 tick = 7; tick < 15; ++tick) {
        pool.step(tick);
    }
    // SHA-256 of the generators' native digest twelve steps in, the boss's among them, and of
    // what they saved four steps in, which netplay peers must hash alike: a literal moves only
    // when what is visited changes on purpose.
    test::checkPinnedDigests("Generators", pool.digest(), pool.digest(saved),
                             "f8b481b090e35c1dcaa5726ee2cb435aa50493b6e8d3b7de721336d802c8797b",
                             "28953ba7b082845ea4478ba7d83a7087afd3fbef94d55f398f52203034e4c463");
}
} // namespace
