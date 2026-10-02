#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "formats/CritterWad.h"
#include "game/enemies/BossDefinition.h"
#include "game/enemies/CombatantFixture.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/CombatantProjectiles.h"
#include "game/world/LevelWorld.h"
#include "game/world/SafeRocks.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterData data;
    EffectTrees effects;
    CombatantProjectiles projectiles;
    std::vector<std::string> sounds;
    CombatantProjectiles::PlaySound sound = [&](std::string_view name) {
        sounds.emplace_back(name);
    };
    Fixture() {
        const auto root = test::scratchDirectory("critter-projectiles");
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
            {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]},
            {"name":"LOOP","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
            {"name":"HIT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
            "damages":[{"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,
                "sfxIndex":0,"sfx":2,"morph":1,"morphEnd":2,"morphLife":0.5},
                {"type":1,"flags":2097184,"behaviorFlags":9,"radius":0.5,"damage":12,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
                {"type":1,"flags":1048576,"behaviorFlags":9,"radius":0.5,"damage":60,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":0,"morph":1,"morphLife":15},
                {"type":1,"flags":67108864,"behaviorFlags":9,"radius":0.5,"maxDistance":2,"damage":1,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
                {"type":1,"flags":1,"behaviorFlags":9,"radius":0.5,"damage":20,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":4,"sfx":2},
                {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":5,"sfx":2},
                {"type":8,"flags":67108864,"radius":3,"maxDistance":3,"damage":1,
                 "sfxIndex":0,"morph":1,"morphEnd":2,"morphLife":5},
                {"type":1,"flags":3145760,"behaviorFlags":9,"radius":0.5,"damage":12,
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2}],
            "sounds":[{"name":"SHOT","levelFormat":"S_%cSHOT"},{"name":"LOOP"},
                      {"name":"HIT","flags":16,"levelFormat":"S_%cHIT"},{"name":"LOOP","life":100},
                      {"name":"SHOT","flags":131072},{"name":"SHOT","flags":4194304}]})");
        test::convertModelFixture(root);
        REQUIRE(archive.load(root));
        REQUIRE(data.load(root / "critter.json"));
    }
    void launch() {
        CombatShot shot;
        shot.data = &data;
        shot.damageIndex = 0;
        shot.origin = {0, 3, 0};
        shot.target = Vec3{0, 3, 30};
        shot.realm = 'C';
        shot.damageScale = 2;
        projectiles.launch(shot, archive, device, effects, sound);
    }
    void step(f32 seconds, std::span<const EnemyView> players = {},
              const WorldCollision* collision = nullptr, std::span<const MissileStop> items = {}) {
        effects.update(seconds);
        projectiles.update(seconds, collision, players, device, effects, sound, items);
    }
};

TEST_CASE("combatant shots impact items without reflecting or hitting sheltered players",
          "[game][boss-projectiles][safe-rocks]") {
    Fixture f;
    const bool rock = GENERATE(false, true);
    Obstacle cover;
    cover.centre = {0, 0, 2};
    cover.height = 8;
    cover.halfAcross = 5;
    cover.halfAlong = 0.5f;
    const std::array stops{MissileStop{.box = cover, .rock = rock ? 3 : -1, .rockHealth = 90}};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 1; // reflective, but not piercing
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    shot.realm = 'I';
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    const std::array players{EnemyView{0, {0, 0, 5}, 1, 6}};
    f.step(0.25f, players, nullptr, stops);
    CHECK(f.projectiles.count() == 0);
    CHECK(f.projectiles.takeHits().empty());
    CHECK(f.projectiles.takeWorldHits().empty());
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "HIT");
    CHECK(f.effects.effect(0).position.z ==
          Approx(1.0f)); // full item radius, not world half-radius
    CHECK(f.sounds == std::vector<std::string>{"S_IHIT"});
    const auto hits = f.projectiles.takeRockHits();
    REQUIRE(hits.size() == (rock ? 1U : 0U));
    if (rock) {
        CHECK(hits.front().rock == 3);
        CHECK(hits.front().damage == 12);
    }
    CHECK(f.projectiles.takeRockHits().empty());
}

TEST_CASE("piercing combatant shots stop at surviving cover and pass destroyed cover",
          "[game][boss-projectiles][safe-rocks]") {
    Fixture f;
    const bool survives = GENERATE(false, true);
    Obstacle cover;
    cover.centre = {0, 0, 4};
    cover.height = 8;
    cover.cylinderRadius = 1;
    const std::array stops{MissileStop{.box = cover, .rock = 2, .rockHealth = survives ? 90 : 30}};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 2; // 60 damage, piercing
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.step(0.3f, {}, nullptr, stops);
    const auto hits = f.projectiles.takeRockHits();
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().damage == 60);
    CHECK(f.projectiles.count() == (survives ? 0U : 1U));
    if (survives) {
        CHECK(f.effects.count() == 0); // surviving cover clears fxhit for DMG_SUPER
    } else {
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).position.z > 8);
    }
}

TEST_CASE("combatant item sweeps choose the nearest cover and account for earlier shots",
          "[game][boss-projectiles][safe-rocks]") {
    Fixture f;
    Obstacle cover;
    cover.centre = {0, 0, 2};
    cover.height = 8;
    cover.halfAcross = 5;
    cover.halfAlong = 0.01f;
    auto farther = cover;
    farther.centre.z += 0.1f;
    const std::array stops{MissileStop{.box = farther, .rock = 1, .rockHealth = 90},
                           MissileStop{.box = cover, .rock = 0, .rockHealth = 10}};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 1;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.step(0.2f, {}, nullptr, stops);
    const auto hits = f.projectiles.takeRockHits();
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].rock == 0);
    CHECK(hits[1].rock == 1);
    CHECK(f.projectiles.count() == 0);
}

TEST_CASE("retail Yeti ice attacks damage I5 cover and only pierce a destroyed rock",
          "[game][boss-projectiles][safe-rocks][yeti][assets]") {
    const s32 attack = GENERATE(2, 3, 9, 10, 13, 14);
    const s32 fps = GENERATE(30, 60);
    CAPTURE(attack, fps);
    Fixture f;
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELI5/WORLDS.PS2");
    test::assetOrSkip("ITEMS/LEVELI5/objects.ngc");
    REQUIRE(f.data.load(root / "CRITTER/YETI.WAD"));
    REQUIRE(f.archive.load(root / "MONSTERS/YETI"));
    WorldLayout layout;
    ItemArchive items;
    REQUIRE(layout.load(root / "LEVELS/LEVELI5"));
    REQUIRE(items.load(root / "ITEMS/LEVELI5"));
    SafeRocks rocks;
    REQUIRE(rocks.bind(f.device, layout, items));
    rocks.setPlayerCount(1);
    rocks.hideForEruptions();
    rocks.activate(3);
    const auto& rock = rocks.rock(3);
    REQUIRE(rock.health == 90);
    const std::array stops{MissileStop{
        .box = rock.obstacle, .rock = 3, .rockHealth = rock.health, .rockArmor = rock.armor}};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = attack;
    shot.origin = rock.obstacle.centre + Vec3{0, 3, -8};
    shot.target = shot.origin + Vec3{0, 0, 30};
    shot.realm = 'I';
    // DAMG launch offsets have already been applied by the actor before it emits the shot.
    shot.endVisual = bossDefinition("YETI").projectileEndVisual;
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    std::vector<RockHit> hits;
    for (s32 frame = 0; frame < fps && hits.empty(); ++frame) {
        f.step(1.0f / static_cast<f32>(fps), {}, nullptr, stops);
        hits = f.projectiles.takeRockHits();
    }
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].rock == 3);
    CHECK(hits[0].damage == f.data.damage(attack)->damage);
    const bool destroyed = rocks.strike(3, hits[0].damage);
    CHECK(destroyed == (attack == 9 || attack == 10));
    CHECK(f.projectiles.count() == (destroyed ? 1U : 0U));
    if (!destroyed) {
        REQUIRE_FALSE(f.sounds.empty());
        CHECK(f.sounds.back() == "S_YETIPHIT");
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == "ATTACK8FXC");
        f.effects.update(1.0f / static_cast<f32>(fps));
        f.effects.draw(f.device, Mat4{1}, WorldLighting{});
        CHECK_FALSE(f.device.draws.empty());
    }
    CHECK(f.projectiles.takeHits().empty());
}

TEST_CASE("projectile end visuals are opt-in and never add damage or completion callbacks",
          "[game][boss-projectiles][projectile-end]") {
    const bool visibleEnd = GENERATE(false, true);
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 7;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    shot.realm = 'I';
    shot.endVisual = visibleEnd;
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    bool cleared = false;
    usize rockHits = 0;
    SECTION("natural expiry") {
        f.step(101);
    }
    SECTION("surviving cover") {
        Obstacle cover;
        cover.centre = {0, 0, 2};
        cover.height = 8;
        cover.halfAcross = 5;
        cover.halfAlong = 0.5f;
        const std::array stops{MissileStop{.box = cover, .rock = 0, .rockHealth = 90}};
        f.step(0.1f, {}, nullptr, stops);
        rockHits = 1;
    }
    SECTION("shortened rebound lifetime") {
        WorldCollision world;
        CollisionTriangle wall;
        wall.normal = {0, 0, -1};
        wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
        world.build({wall});
        f.step(0.1f, {}, &world);
        REQUIRE(f.projectiles.count() == 1);
        REQUIRE(f.effects.effect(0).name == "LOOP");
        REQUIRE(f.effects.effect(0).secondsLeft == 10);
        REQUIRE(f.sounds.empty());
        f.step(11);
    }
    SECTION("teardown is silent") {
        f.projectiles.clear(f.effects);
        cleared = true;
    }
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.projectiles.takeHits().empty());
    REQUIRE(f.projectiles.takeRockHits().size() == rockHits);
    REQUIRE(f.projectiles.takeGenerators().empty());
    REQUIRE(f.projectiles.takeSummons().empty());
    const bool shows = visibleEnd && !cleared;
    REQUIRE(f.effects.count() == (shows ? 1U : 0U));
    if (shows) {
        CHECK(f.effects.effect(0).name == "HIT");
        f.effects.draw(f.device, Mat4{1}, WorldLighting{});
        CHECK_FALSE(f.device.draws.empty());
    }
    CHECK(f.sounds.size() == (shows || rockHits != 0 ? 1U : 0U));
    const usize sounds = f.sounds.size();
    const std::array players{EnemyView{0, {0, 0, 0}, 100, 100}};
    f.step(2, players);
    CHECK(f.effects.count() == 0);
    CHECK(f.projectiles.takeHits().empty());
    CHECK(f.projectiles.takeRockHits().empty());
    CHECK(f.projectiles.takeGenerators().empty());
    CHECK(f.projectiles.takeSummons().empty());
    CHECK(f.sounds.size() == sounds);
}

TEST_CASE("Yeti iceball expiry plays the authored break once without splash damage",
          "[game][boss-projectiles][yeti][projectile-end][assets]") {
    const s32 attack = GENERATE(2, 3, 9, 10, 13, 14);
    const s32 fps = GENERATE(30, 120);
    const bool curbed = GENERATE(false, true);
    CAPTURE(attack, fps, curbed);
    Fixture f;
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    REQUIRE(f.data.load(root / "CRITTER/YETI.WAD"));
    REQUIRE(f.archive.load(root / "MONSTERS/YETI"));
    REQUIRE(bossDefinition("YETI").projectileEndVisual);
    for (s32 kind = 34; kind <= 44; ++kind) {
        CHECK(bossDefinition(bossNameOf(kind)).projectileEndVisual == (kind == 39));
    }
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = attack;
    shot.origin = {0, 10, 0};
    shot.target = Vec3{0, 10, 30};
    shot.realm = 'I';
    shot.endVisual = bossDefinition("YETI").projectileEndVisual;
    shot.birthLife = curbed ? 0.25f : 0;
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    const f32 dt = 1.0f / static_cast<f32>(fps);
    f.step(dt);
    REQUIRE(f.effects.effect(0).name == "ATTACK8FXB");
    const Vec3 endPosition = f.effects.effect(0).position;
    const auto life = f.effects.remaining(f.effects.effect(0).id);
    REQUIRE(life.has_value());
    // End the authored/curbed timer without introducing a floor or player impact.
    f.effects.update(*life + dt);
    const std::array players{EnemyView{0, endPosition - Vec3{0, 3, 0}, 1, 6}};
    f.projectiles.update(dt, nullptr, players, f.device, f.effects, f.sound);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 1);
    const auto& end = f.effects.effect(0);
    REQUIRE(end.name == "ATTACK8FXC");
    CHECK(end.position == endPosition);
    REQUIRE(end.tree != nullptr);
    REQUIRE_FALSE(end.tree->sequences.empty());
    const auto duration = f.effects.remaining(end.id);
    REQUIRE(duration.has_value());
    CHECK(*duration ==
          Approx(static_cast<f32>(end.tree->sequences[0].frames) * end.player.secondsPerFrame()));
    CHECK_FALSE(end.timed);
    f.effects.update(dt);
    f.effects.draw(f.device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(f.device.draws.empty());
    CHECK(f.sounds == std::vector<std::string>{"S_YETIPHIT"});
    f.step(*duration + dt, players);
    CHECK(f.effects.count() == 0);
    CHECK(f.projectiles.takeHits().empty());
    CHECK(f.projectiles.takeGenerators().empty());
    CHECK(f.projectiles.takeSummons().empty());
    CHECK(f.sounds.size() == 1);
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.projectiles.clear(f.effects);
    CHECK(f.effects.count() == 0);
    CHECK(f.sounds.size() == 1);
}

TEST_CASE("combatant item contacts respect ignore-world and nearer player contacts",
          "[game][boss-projectiles][safe-rocks]") {
    Fixture f;
    const bool ignoresWorld = GENERATE(false, true);
    const bool playerFirst = GENERATE(false, true);
    const auto root = test::scratchDirectory("combatant-item-flags");
    writeTextFile(root / "critter.json",
                  R"({"types":[{"moveCount":1}],"moves":[{}],"descriptors":[{}],
        "damages":[{"type":1,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,
        "sfxIndex":0,"sfx":1,"behaviorFlags":)" +
                      std::to_string(ignoresWorld ? 73 : 9) + R"(}],
        "sounds":[{"name":"LOOP","life":10},{"name":"HIT"}]})");
    REQUIRE(f.data.load(root / "critter.json"));
    Obstacle cover;
    cover.centre = {0, 0, 6};
    cover.height = 8;
    cover.halfAcross = 5;
    cover.halfAlong = 0.5f;
    const std::array stops{MissileStop{.box = cover, .rock = 0, .rockHealth = 90}};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 0;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    const std::array players{EnemyView{0, {0, 0, playerFirst ? 3.0f : 10.0f}, 1, 6}};
    f.step(0.5f, players, nullptr, stops);
    const bool rockHit = !ignoresWorld && !playerFirst;
    CHECK(f.projectiles.takeRockHits().size() == (rockHit ? 1U : 0U));
    CHECK(f.projectiles.takeHits().size() == (rockHit ? 0U : 1U));
    CHECK(f.projectiles.count() == 0);
}

TEST_CASE("arrow orientation follows live velocity while ordinary projectiles keep their pose",
          "[boss-projectiles][garm]") {
    for (const bool arrow : {false, true}) {
        Fixture f;
        const auto root = test::scratchDirectory("directed-projectile");
        writeTextFile(root / "critter.json",
                      R"({"types":[{"moveCount":1}],"moves":[{}],"descriptors":[{}],
            "damages":[{"type":1,"minSpeed":30,"maxSpeed":30,"gravity":6,
            "sfxIndex":0,"behaviorFlags":9,"flags":)" +
                          std::to_string(arrow ? 0x20000 : 0) +
                          R"(}],"sounds":[{"name":"LOOP","life":3}]})");
        REQUIRE(f.data.load(root / "critter.json"));
        CombatShot shot;
        shot.data = &f.data;
        shot.damageIndex = 0;
        shot.origin = {0, 20, 0};
        shot.target = Vec3{10, 0, 30};
        f.projectiles.launch(shot, f.archive, f.device, f.effects, {});
        REQUIRE(f.effects.count() == 1);
        REQUIRE(f.effects.effect(0).flightDirection.has_value() == arrow);
        const Vec3 initial = glm::normalize(*shot.target - shot.origin) * 30.0f;
        f.step(0.25f);
        REQUIRE(f.effects.count() == 1);
        if (arrow) {
            REQUIRE(f.effects.effect(0).flightDirection);
            CHECK(glm::distance(*f.effects.effect(0).flightDirection, initial + Vec3{0, -1.5f, 0}) <
                  0.001f);
        } else {
            CHECK_FALSE(f.effects.effect(0).flightDirection);
        }
    }
}

TEST_CASE("projectile impacts expand their damage then go harmless before the artwork ends",
          "[boss-projectiles][projectile-impact]") {
    Fixture f;
    const auto root = test::scratchDirectory("projectile-impact-area");
    writeTextFile(root / "critter.json", R"({"types":[{"moveCount":1}],"moves":[{}],
      "descriptors":[{}],"damages":[{"type":1,"minSpeed":30,"maxSpeed":30,
      "radius":0.5,"maxDistance":10,"minDot":-1,"damage":50,"flags":32,
      "behaviorFlags":9,"sfxIndex":0,"sfx":1}],
      "sounds":[{"name":"LOOP","life":3},{"name":"HIT","life":1}]})");
    REQUIRE(f.data.load(root / "critter.json"));
    f.launch();
    EnemyView contact;
    contact.player = 0;
    contact.position = {0, 0, 2};
    contact.radius = 1;
    contact.height = 6;
    f.step(0.05f, std::span{&contact, 1});
    const auto direct = f.projectiles.takeHits();
    REQUIRE(direct.size() == 1);
    CHECK(direct[0].damage == 100);
    REQUIRE(f.projectiles.count() == 1);
    EnemyView bystander = contact;
    bystander.player = 1;
    bystander.position = {7, 0, 0.5f};
    f.step(0.05f, std::span{&bystander, 1});
    CHECK(f.projectiles.takeHits().empty());
    f.step(0.4f, std::span{&bystander, 1});
    const auto splash = f.projectiles.takeHits();
    REQUIRE(splash.size() == 1);
    CHECK(splash[0].player == 1);
    CHECK(splash[0].damage == Approx(33));
    CHECK(splash[0].repeatGap == Approx(0.616667f));
    f.step(0.25f, std::span{&bystander, 1});
    CHECK(f.projectiles.takeHits().empty());
    CHECK(f.effects.count() == 1);
    f.step(0.31f, std::span{&bystander, 1});
    CHECK(f.projectiles.count() == 0);
    CHECK(f.effects.count() == 0);
}

TEST_CASE("timed-out explosive shots enter their impact phase without a collision",
          "[boss-projectiles][projectile-impact]") {
    Fixture f;
    const auto root = test::scratchDirectory("timed-projectile-impact");
    writeTextFile(root / "critter.json", R"({"types":[{"moveCount":1}],"moves":[{}],
      "descriptors":[{}],"damages":[{"type":1,"radius":0.5,"maxDistance":10,
      "minDot":-1,"damage":50,"sfxIndex":0,"sfx":1}],
      "sounds":[{"name":"LOOP","life":0.1},{"name":"HIT","life":1}]})");
    REQUIRE(f.data.load(root / "critter.json"));
    f.launch();
    f.step(0.11f);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "HIT");
    f.projectiles.clear(f.effects);
    CHECK(f.effects.count() == 0);
}

TEST_CASE("shipped projectile cues are trees and their only custom links are particle trails",
          "[boss-projectiles][projectile-trail][assets]") {
    const auto root = test::assetOrSkip("CRITTER/DRAGON.WAD").parent_path();
    usize shots = 0;
    usize trails = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".WAD" && entry.path().extension() != ".wad") {
            continue;
        }
        CritterData data;
        REQUIRE(data.load(entry.path()));
        // Attack tables are shared by every TYPE in an archive.
        for (s32 i = 0; const auto* damage = data.damage(i); ++i) {
            if (damage->type != AttackDefinition::kProjectile) {
                continue;
            }
            const auto* cue = data.sound(damage->sound);
            if (cue == nullptr) {
                continue;
            }
            CAPTURE(entry.path().filename().string(), i, cue->tree);
            CHECK((cue->flags & 0xF000000U) == 0);
            ++shots;
            if (const auto* link = data.sound(cue->link)) {
                CHECK((link->flags & (0xF000000U | 0x4000U)) == 0x02004000U);
                ++trails;
            }
        }
    }
    CHECK(shots > 50);
    CHECK(trails > 0);
}

TEST_CASE("linked projectile fire emits behind its moving parent and stops at the authored time",
          "[boss-projectiles][projectile-trail]") {
    Fixture f;
    const auto root = test::scratchDirectory("linked-projectile-data");
    writeTextFile(root / "critter.json", R"({"types":[{"moveCount":1}],"moves":[{}],
        "descriptors":[{}],"damages":[{"type":1,"minSpeed":30,"maxSpeed":30,
        "sfxIndex":0,"behaviorFlags":9}],"sounds":[{"name":"LOOP","life":3,"link":1},
        {"name":"WHITE","flags":33570816,"life":1,"rate":5,"custom1":500,"link":-1}]})");
    REQUIRE(f.data.load(root / "critter.json"));
    f.launch();
    REQUIRE(f.effects.count() == 1);
    const auto& trail = f.effects.effect(0).trails;
    REQUIRE(trail.size() == 1);
    const auto& emitter = trail.emitter(0);
    const auto& d = emitter.descriptor();
    CHECK(d.rate[0] == 5);
    CHECK(d.speed == Approx(5.0f / 30));
    CHECK(d.emitFrames == 30);
    CHECK(d.fadeFrames == 1);
    CHECK(d.particleLife == 6);
    CHECK(d.particleFade == 6);
    CHECK(d.width.lifeStart == 0.5f);
    CHECK(d.alpha.fadeEnd == 0);
    CHECK(d.additive);
    CHECK_FALSE(d.depthWrite);
    CHECK_FALSE(d.dynamic);
    f.step(1.0f / 30);
    REQUIRE(trail.particleCount() == 5);
    const Vec3 born = emitter.particles().front().origin;
    f.step(1.0f / 30);
    CHECK(emitter.particles().front().origin == born);
    CHECK(Vec3{emitter.node()[3]}.z > born.z);
    for (s32 frame = 2; frame < 44; ++frame) {
        f.step(1.0f / 30);
    }
    CHECK(trail.particleCount() == 0);
    CHECK(f.projectiles.count() == 1);
    f.projectiles.clear(f.effects);
    CHECK(f.effects.count() == 0);
}

TEST_CASE("Dragon fireball records attach their actual linked particle texture",
          "[boss-projectiles][projectile-trail][assets]") {
    const auto root = test::assetOrSkip("CRITTER/DRAGON.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/DRAGON/ANIM.PS2");
    CritterData data;
    ItemArchive archive;
    REQUIRE(data.load(root / "CRITTER/DRAGON.WAD"));
    REQUIRE(archive.load(root / "MONSTERS/DRAGON"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    CombatantProjectiles projectiles;
    s32 covered = 0;
    for (s32 i = 0; data.damage(i) != nullptr; ++i) {
        const auto* damage = data.damage(i);
        if (damage->type != AttackDefinition::kProjectile || damage->sound != 11) {
            continue;
        }
        CombatShot shot;
        shot.data = &data;
        shot.damageIndex = i;
        shot.origin = {0, 10, 0};
        shot.target = Vec3{0, 10, 30};
        projectiles.launch(shot, archive, device, effects, {});
        REQUIRE(projectiles.count() == 1);
        REQUIRE(effects.effect(0).trails.size() == 1);
        const auto texture = archive.textures.find("FBALL_LP00");
        REQUIRE(texture);
        CHECK(effects.effect(0).trails.textureOf(0) == &archive.textures.texture(device, *texture));
        effects.update(1.0f / 30);
        CHECK(effects.effect(0).trails.particleCount() == 5);
        projectiles.clear(effects);
        ++covered;
    }
    CHECK(covered == 7);
}

TEST_CASE("planted hand traps hold a fixed position, allow escape, and end their damage",
          "[boss-projectiles][lich]") {
    const s32 frameRate = GENERATE(30, 60, 120);
    const f32 dt = 1.0f / static_cast<f32>(frameRate);
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 6;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{80, 3, 80}; // never a velocity destination
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    REQUIRE(f.projectiles.count() == 1);
    std::array<EnemyView, 1> players{{{0, {0, 0, 0}, 1, 6}}};
    usize hits = 0;
    for (s32 frame = 0; frame < frameRate; ++frame) {
        f.step(dt, players);
        for (const auto& hit : f.projectiles.takeHits()) {
            REQUIRE(hit.damage == 1);
            REQUIRE(hit.flags == PlayerImpact::kSticky);
            REQUIRE(hit.direction == Vec3{0});
            ++hits;
        }
    }
    REQUIRE(hits >= 29);
    REQUIRE(hits <= 30);
    players[0].position.x = 20;
    f.step(1, players);
    REQUIRE(f.projectiles.takeHits().empty());
    players[0].position.x = 0;
    f.step(0.5f, players);
    REQUIRE_FALSE(f.projectiles.takeHits().empty());
    f.step(4, players); // coarse frame crosses the hold's end
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.projectiles.takeHits().size() <= 79); // only the remaining 2.6 seconds
    REQUIRE(f.sounds.size() == 2);                  // birth and end; LOOP has no cue
    f.step(1, players);
    REQUIRE(f.projectiles.takeHits().empty());
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("boss projectiles report water hits but rebounds do not play impact sounds",
          "[boss-projectiles][projectile-impact]") {
    const bool reflects = GENERATE(false, true);
    Fixture f;
    WorldCollision world;
    CollisionTriangle water;
    water.object = 73;
    water.objectFlags = WorldCollision::kLiquidSurface;
    water.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    world.build({water});
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = reflects ? 1 : 0;
    shot.origin = {0, 1, 0};
    shot.target = Vec3{0, -1, 10};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    std::vector<CombatantWorldHit> hits;
    for (s32 frame = 0; frame < 60 && hits.empty(); ++frame) {
        f.step(1.0f / 120, {}, &world);
        hits = f.projectiles.takeWorldHits();
    }
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().object == 73);
    CHECK(hits.front().splash == !reflects);
    CHECK(std::ranges::find(f.sounds, "S_HIT") == f.sounds.end());
    CHECK(f.projectiles.count() == (reflects ? 1 : 0));
    CHECK(f.projectiles.takeWorldHits().empty());
    f.projectiles.clear(f.effects);
    CHECK(f.projectiles.takeWorldHits().empty());
}

TEST_CASE("generator shots leave one stage placement on expiry, but never on clear",
          "[boss-projectiles][spider]") {
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 4;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.step(0.05f);
    REQUIRE(f.projectiles.takeGenerators().empty());
    f.step(0.1f);
    REQUIRE(f.projectiles.count() == 0);
    const auto generators = f.projectiles.takeGenerators();
    REQUIRE(generators.size() == 1);
    REQUIRE(generators[0][3].z == Approx(1.5f));
    REQUIRE(f.projectiles.takeGenerators().empty());
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.projectiles.clear(f.effects);
    REQUIRE(f.projectiles.takeGenerators().empty());
}

TEST_CASE("Lich's shipped hand trap aligns to the floor and morphs for five seconds",
          "[boss-projectiles][lich][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    Fixture f;
    REQUIRE(f.data.load(root / "CRITTER/LICH.WAD"));
    REQUIRE(f.archive.load(root / "MONSTERS/LICH"));
    WorldCollision floor;
    CollisionTriangle triangle;
    triangle.normal = {0, 1, 0};
    triangle.vertices = {Vec3{-100, 0, -100}, Vec3{0, 0, 100}, Vec3{100, 0, -100}};
    floor.build({triangle});
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 17;
    shot.origin = {0, 3, 0};
    REQUIRE(f.data.damage(17)->type == AttackDefinition::kTargetArea);
    REQUIRE(f.data.damage(17)->flags == PlayerImpact::kSticky);
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound, &floor);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.effect(0).name == "ATK14GENFX");
    REQUIRE(f.effects.effect(0).position.y == Approx(0.1f));
    const f32 birth = *f.effects.remaining(f.effects.effect(0).id);
    REQUIRE(birth > 0);
    f.step(birth + 0.01f);
    REQUIRE(f.projectiles.count() == 1);
    bool foundLoop = false;
    for (usize i = 0; i < f.effects.count(); ++i) {
        if (f.effects.effect(i).name == "ATK14LPFX") {
            foundLoop = true;
            REQUIRE(f.effects.effect(i).position.y == Approx(0.1f));
        }
    }
    REQUIRE(foundLoop);
    f.step(4.98f);
    REQUIRE(f.projectiles.count() == 1);
    f.step(0.02f);
    REQUIRE(f.projectiles.count() == 0);
    bool foundEnd = false;
    for (usize i = 0; i < f.effects.count(); ++i) {
        foundEnd = foundEnd || f.effects.effect(i).name == "ATK14ENDFX";
    }
    REQUIRE(foundEnd);
    REQUIRE(f.sounds == std::vector<std::string>{"S_LICHCONJ", "S_LICHHAND"});
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("summoning shots invoke one callback on contact or expiration, never on clear",
          "[boss-projectiles][garm]") {
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 5;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    SECTION("contact does not wait for the impact animation") {
        const std::array<EnemyView, 1> players{{{0, {0, 0, 1}, 1, 6}}};
        f.step(0.01f, players);
    }
    SECTION("expiration also summons") {
        f.step(0.2f);
    }
    REQUIRE(f.projectiles.takeSummons().size() == 1);
    f.step(2);
    REQUIRE(f.projectiles.takeSummons().empty());
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.projectiles.clear(f.effects);
    REQUIRE(f.projectiles.takeSummons().empty());
}

TEST_CASE("Lich and Spider Queen egg records request stage generators",
          "[boss-projectiles][spider][lich][assets]") {
    const bool hitPlayer = GENERATE(false, true);
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    for (const std::string name : {"LICH", "DRIDER"}) {
        DYNAMIC_SECTION(name) {
            test::assetOrSkip("MONSTERS/" + name + "/ANIM.PS2");
            CritterData data;
            REQUIRE(data.load(root / "critter" / (name + ".WAD")));
            ItemArchive archive;
            REQUIRE(archive.load(root / "MONSTERS" / name));
            test::FakeRenderDevice device;
            EffectTrees effects;
            CombatantProjectiles projectiles;
            usize eggs = 0;
            for (usize i = 0; i < data.damages().size(); ++i) {
                const auto& damage = data.damages()[i];
                const auto* cue = data.sound(damage.sound);
                if (cue == nullptr || (cue->flags & 0x20000U) == 0) {
                    continue;
                }
                CombatShot shot;
                shot.data = &data;
                shot.damageIndex = static_cast<s32>(i);
                shot.origin = {0, 3, 0};
                projectiles.launch(shot, archive, device, effects, {});
                REQUIRE(projectiles.count() == 1);
                const std::vector<EnemyView> players =
                    hitPlayer ? std::vector<EnemyView>{{0, {0, -10, 0}, 10, 30}}
                              : std::vector<EnemyView>{};
                usize directHits = 0;
                usize areaHits = 0;
                for (s32 frame = 0; frame < 900 && projectiles.count() > 0; ++frame) {
                    effects.update(1.0f / 30);
                    projectiles.update(1.0f / 30, nullptr, players, device, effects, {});
                    for (const auto& hit : projectiles.takeHits()) {
                        if (frame == 0) {
                            ++directHits;
                        } else {
                            // The egg's impact expands before leaving a generator.
                            // These contacts carry immunity time; they are not web ticks.
                            ++areaHits;
                            if (hit.damage > 2) {
                                REQUIRE(hit.repeatGap > 0);
                            } else {
                                REQUIRE((hit.flags & 0x170U) == 0); // faint tail cannot stagger
                            }
                        }
                        REQUIRE((hit.flags & PlayerImpact::kSticky) == 0);
                        const PlayerImpact impact{hit.flags, hit.direction};
                        REQUIRE(impact.reaction(hit.damage, 0, false) != PlayerDeed::Webbed);
                    }
                }
                REQUIRE(directHits == (hitPlayer ? 1 : 0));
                REQUIRE((areaHits > 0) == hitPlayer);
                REQUIRE(projectiles.takeGenerators().size() == 1);
                ++eggs;
            }
            REQUIRE(eggs == (name == "LICH" ? 1 : 2));
            projectiles.clear(effects);
        }
    }
}

TEST_CASE("Garm eye ribbons follow their descending trajectory rather than horizontal yaw",
          "[boss-projectiles][garm][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GARM.WAD").parent_path().parent_path();
    CritterData data;
    REQUIRE(data.load(root / "CRITTER/GARM.WAD"));
    ItemArchive archive;
    REQUIRE(archive.load(root / "MONSTERS/GARM"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    CombatantProjectiles shots;
    CombatShot shot;
    shot.data = &data;
    shot.damageIndex = 3;
    shot.origin = {0, 16, 0};
    shot.target = Vec3{15, 3, 30};
    shots.launch(shot, archive, device, effects, {});
    REQUIRE(shots.count() == 1);
    REQUIRE(effects.count() == 1);
    const auto& effect = effects.effect(0);
    REQUIRE(effect.flightDirection);
    CHECK(glm::dot(glm::normalize(*effect.flightDirection),
                   glm::normalize(*shot.target - shot.origin)) > 0.9999f);
    WorldCamera view;
    view.position = {10, 25, 55};
    view.yaw = 3.0f;
    view.pitch = 0.3f;
    const auto camera = CameraFrame::of(view);
    effects.draw(device, Mat4{1}, {}, &camera);
    REQUIRE(device.draws.size() == 2);
    const Vec3 heading = glm::normalize(*shot.target - shot.origin);
    // Top-facing ribbons keep z. Their actual world vertices must span the
    // downward flight axis, not the horizontal plane from the old yaw pose.
    const auto& vertices = device.draws[0].vertices;
    f32 longest = 0;
    Vec3 along{0};
    for (const auto& a : vertices) {
        for (const auto& b : vertices) {
            const Vec3 delta = b.position - a.position;
            if (glm::length(delta) > longest) {
                longest = glm::length(delta);
                along = delta;
            }
        }
    }
    REQUIRE(longest > 20);
    CHECK(std::abs(glm::dot(glm::normalize(along), heading)) > 0.98f);
    shots.clear(effects);
}

TEST_CASE("Garm's two body-break projectiles request summons without player damage",
          "[boss-projectiles][garm][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GARM.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GARM/ANIM.PS2");
    CritterData data;
    REQUIRE(data.load(root / "CRITTER/GARM.WAD"));
    ItemArchive archive;
    REQUIRE(archive.load(root / "MONSTERS/GARM"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    CombatantProjectiles projectiles;
    WorldCollision floor;
    CollisionTriangle triangle;
    triangle.normal = {0, 1, 0};
    triangle.vertices = {Vec3{-100, 0, -100}, Vec3{0, 0, 100}, Vec3{100, 0, -100}};
    floor.build({triangle});
    for (const s32 index : {1, 2}) {
        CombatShot shot;
        shot.data = &data;
        shot.damageIndex = index;
        shot.origin = {0, 6, 0};
        projectiles.launch(shot, archive, device, effects, {});
        REQUIRE(projectiles.count() == 1);
        const std::array<EnemyView, 1> players{{{0, {0, 0, 0}, 1, 12}}};
        for (s32 frame = 0; frame < 900 && projectiles.count() > 0; ++frame) {
            effects.update(1.0f / 30);
            projectiles.update(1.0f / 30, &floor, players, device, effects, {});
        }
        REQUIRE(projectiles.takeSummons().size() == 1);
        REQUIRE(projectiles.takeGenerators().empty());
        REQUIRE(projectiles.takeHits().empty());
    }
    projectiles.clear(effects);
}

TEST_CASE("generator shots finish their impact before leaving a stage placement",
          "[boss-projectiles][spider]") {
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 4;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    const std::array<EnemyView, 1> players{{{0, {0, 0, 1}, 1, 6}}};
    f.step(0.01f, players);
    REQUIRE(f.projectiles.takeHits().size() == 1);
    REQUIRE(f.projectiles.takeGenerators().empty());
    REQUIRE(f.projectiles.count() == 1);
    f.step(2, players);
    REQUIRE(f.projectiles.takeGenerators().size() == 1);
    REQUIRE(f.projectiles.takeHits().empty());
    REQUIRE(f.projectiles.count() == 0);
}

void checkWebEscape(s32 fps, bool animated) {
    Fixture f;
    const f32 dt = 1.0f / static_cast<f32>(fps);
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    player.actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    player.actor.save().progress().health = 1000;
    if (animated) {
        const auto root = test::assetOrSkip("PLAYERS/WAR/RED/objects.ngc")
                              .parent_path()
                              .parent_path()
                              .parent_path()
                              .parent_path();
        player.figure = PlayerFigure::load(f.device, root, player.actor.save());
        REQUIRE(player.figure != nullptr);
        for (s32 frame = 0; frame < 150; ++frame) {
            player.figure->animate(0, 2, 1.0f / 30);
        }
        REQUIRE_FALSE(player.figure->animator().entering());
    }
    WorldCollision world;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{0, 0, 100}, Vec3{100, 0, -100}};
    floor.normal = {0, 1, 0};
    world.build({floor});
    std::array<PlayInput, 1> inputs;
    inputs[0].move = {Vec2{0, 1}, 1};
    inputs[0].attack = true;
    PlayerHealth health;
    const PlayerHealth::Events healthEvents{.block = [](f32, f32) {},
                                            .sound = [](std::string_view) {},
                                            .cry = [](std::string_view) {},
                                            .named = [](std::string_view, f32) {},
                                            .learnBlock = {}};
    PartyMotion::Events motion;
    motion.perform = [](usize, PartyMotion::Action) {};
    motion.select = [](usize, const SelectorInput&, s32) {};
    motion.advanceTurbo = [](usize, s32, f32) {};
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 3;
    shot.origin = {0, 2, -1};
    shot.target = Vec3{0, 2, 5};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    s32 hits = 0;
    s32 healthAfterEscape = 0;
    for (s32 frame = 0; frame < fps * 5; ++frame) {
        const std::array<EnemyView, 1> views{
            {{0, player.actor.position(), player.actor.radius(), player.actor.height()}}};
        f.step(dt, views, &world);
        for (const auto& hit : f.projectiles.takeHits()) {
            ++hits;
            health.hurt(player, hit.damage, HurtKind::Pierce, true, false, 1, healthEvents,
                        {hit.flags, hit.direction});
        }
        const s32 ticks = (frame + 1) * 60 / fps - frame * 60 / fps;
        PartyMotion::step(players, inputs, false, 0, ticks, dt, world, motion);
        if (frame == fps * 3) {
            healthAfterEscape = player.actor.save().health();
            REQUIRE(player.actor.position().z > 3);
            inputs[0].attack = false;
        }
    }
    REQUIRE(hits > 1);
    REQUIRE(hits < 90);
    REQUIRE(player.actor.save().health() == healthAfterEscape);
    REQUIRE(player.life == PlayerLife::Standing);
    REQUIRE(f.projectiles.count() == 1); // Escaped a live trap; it did not merely expire.
    if (player.figure != nullptr) {
        REQUIRE_FALSE(player.figure->animator().webbed());
    }
    f.projectiles.clear(f.effects);
}

TEST_CASE("players can escape a landed sticky projectile and stop taking contact damage",
          "[boss-projectiles][player-impact][party-motion][spider]") {
    checkWebEscape(GENERATE(30, 60, 120), false);
}

TEST_CASE("animated players can walk out of a live web while holding attack",
          "[boss-projectiles][player-impact][party-motion][spider][assets]") {
    checkWebEscape(GENERATE(30, 60, 120), true);
}

TEST_CASE("pass-through snakes survive player contact but still collide with the world",
          "[game][boss-projectiles][wraith]") {
    Fixture f;
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 2;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    const std::array<EnemyView, 2> players{{{7, {0, 0, 2}, 1, 6}, {3, {0, 0, 4}, 1, 6}}};
    f.step(0.2f, players);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.effect(0).name == "LOOP");
    REQUIRE(f.effects.effect(0).position.z == Approx(6));
    const auto hits = f.projectiles.takeHits();
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].player == 7);
    REQUIRE(hits[1].player == 3);
    REQUIRE(hits[0].repeatGap == Approx(0.25f));
    WorldCollision world;
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-10, 0, 8}, Vec3{0, 20, 8}, Vec3{10, 0, 8}};
    world.build({wall});
    f.step(0.2f, {}, &world);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("sticky impacts remain floor-aligned traps for twenty seconds",
          "[game][boss-projectiles][wraith]") {
    const s32 framesPerSecond = GENERATE(30, 60, 120);
    Fixture f;
    WorldCollision world;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}};
    CollisionTriangle other = floor;
    other.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}};
    world.build({floor, other});
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 3;
    shot.origin = {0, 2, 0};
    shot.target = Vec3{0, 0, 10};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    for (s32 i = 0; i < 120 && f.effects.effect(0).name != "HIT"; ++i) {
        f.step(1.0f / 120, {}, &world);
        REQUIRE(f.projectiles.count() == 1);
    }
    REQUIRE(f.effects.effect(0).name == "HIT");
    REQUIRE(f.effects.effect(0).secondsLeft == 20);
    REQUIRE(f.effects.effect(0).position.y == Approx(0.1f));
    const Vec3 at = f.effects.effect(0).position;
    const std::array<EnemyView, 2> players{{{3, {at.x, 0, at.z}, 1, 6}, {8, {50, 0, 50}, 1, 6}}};
    for (s32 i = 0; i < framesPerSecond; ++i) {
        f.step(1.0f / static_cast<f32>(framesPerSecond), players, &world);
    }
    const auto hits = f.projectiles.takeHits();
    REQUIRE(hits.size() == 30);
    for (const auto& hit : hits) {
        REQUIRE(hit.player == 3);
        REQUIRE(hit.damage == 1);
        REQUIRE(hit.flags == 0x4000000);
    }
    REQUIRE(f.effects.effect(0).position == at);
    f.step(19.1f, {}, &world);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("critter projectiles move birth effects morph expire and clear their borrowed effects",
          "[game][boss-projectiles]") {
    Fixture f;
    f.launch();
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.sounds == std::vector<std::string>{"S_CSHOT"});
    f.step(0.04f);
    REQUIRE(f.effects.effect(0).position.z == Approx(1.2f));
    f.step(0.05f);
    REQUIRE(f.effects.effect(0).name == "LOOP");
    f.step(0.51f);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.effect(0).name == "HIT");
    REQUIRE(f.sounds.back() == "S_CHIT");
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0); // the detached morph-end still borrows the archive
    f.launch();
    f.projectiles.clear(f.effects);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 0);
    REQUIRE(f.projectiles.takeHits().empty());
}

TEST_CASE("critter projectiles choose the nearest visible victim regardless of party order",
          "[game][boss-projectiles]") {
    Fixture f;
    f.launch();
    const std::vector<EnemyView> players{EnemyView{7, {0, 0, 5}, 1, 6},
                                         EnemyView{3, {0, 0, 3}, 1, 6},
                                         EnemyView{9, {0, 0, 0}, 1, 6, 1, true}};
    f.step(0.2f, players);
    const auto hits = f.projectiles.takeHits();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == 3);
    REQUIRE(hits[0].damage == 24);
    REQUIRE(hits[0].flags == 0x20);
    REQUIRE(hits[0].direction == Vec3(0, 0, 1));
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.effect(0).name == "HIT");
    REQUIRE(f.effects.effect(0).position.z == Approx(1.5f));
    f.step(0.1f, players);
    REQUIRE(f.projectiles.takeHits().empty());
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0); // an impact is owned after its projectile is removed
}

TEST_CASE("critter projectiles cannot damage players through a world wall",
          "[game][boss-projectiles]") {
    Fixture f;
    WorldCollision world;
    CollisionTriangle wall;
    wall.object = 1;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
    world.build({wall});
    f.launch();
    const std::vector<EnemyView> players{EnemyView{0, {0, 0, 5}, 1, 6}};
    f.step(0.2f, players, &world);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.projectiles.takeHits().empty());
    REQUIRE(f.effects.effect(0).name == "HIT");
}
TEST_CASE("reflecting projectiles rebound from walls without impact bursts and shorten their life",
          "[game][boss-projectiles][yeti]") {
    Fixture f;
    WorldCollision world;
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
    world.build({wall});
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 1;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    f.step(0.1f, {}, &world);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.count() == 1);
    REQUIRE(f.effects.effect(0).name == "LOOP");
    REQUIRE(f.effects.effect(0).position.z < 1);
    REQUIRE(f.effects.effect(0).secondsLeft == 10);
    REQUIRE(f.sounds.empty());
    // A second wall behind the launch point catches the reflected projectile.
    wall.normal = {0, 0, 1};
    wall.vertices = {Vec3{-10, 0, -2}, Vec3{10, 0, -2}, Vec3{0, 20, -2}};
    world.build({wall});
    f.step(0.1f, {}, &world);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.effect(0).secondsLeft == Approx(8.9f));
    f.step(9);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("Yeti iceballs bounce off floors and remain harmful afterward",
          "[game][boss-projectiles][yeti][assets]") {
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    Fixture f;
    REQUIRE(f.data.load(root / "CRITTER/YETI.WAD"));
    REQUIRE(f.archive.load(root / "MONSTERS/YETI"));
    WorldCollision world;
    CollisionTriangle floor;
    floor.objectFlags = WorldObject::kFloor;
    floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}};
    CollisionTriangle other = floor;
    other.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}};
    world.build({floor, other});
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 9;
    shot.origin = {0, 10, 0};
    shot.target = Vec3{0, 0, 30};
    shot.realm = 'I';
    shot.endVisual = bossDefinition("YETI").projectileEndVisual;
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    bool bounced = false;
    f32 previousY = shot.origin.y;
    for (s32 frame = 0; frame < 60 && !bounced; ++frame) {
        f.step(1.0f / 120, {}, &world);
        REQUIRE(f.projectiles.count() == 1);
        REQUIRE(f.effects.effect(0).name == "ATTACK8FXB");
        const f32 y = f.effects.effect(0).position.y;
        bounced = y > previousY;
        previousY = y;
    }
    REQUIRE(bounced);
    REQUIRE(f.projectiles.takeHits().empty());
    const Vec3 ball = f.effects.effect(0).position;
    const std::vector<EnemyView> players{EnemyView{3, {ball.x, 0, ball.z + 5}, 1, 6}};
    for (s32 frame = 0; frame < 120 && f.projectiles.count() != 0; ++frame) {
        f.step(1.0f / 120, players, &world);
    }
    const auto hits = f.projectiles.takeHits();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().player == 3);
    REQUIRE(hits.front().damage == 100);
    REQUIRE(f.effects.effect(0).name == "ATTACK8FXC");
    REQUIRE(f.sounds == std::vector<std::string>{"S_YETIPHIT"});
    f.step(0.1f, players, &world);
    CHECK(f.projectiles.takeHits().empty());
    CHECK(f.sounds.size() == 1);
    f.projectiles.clear(f.effects);
    CHECK(f.effects.count() == 0);
}

TEST_CASE("Yeti mouth-conjured throw survives its launch in the I5 arena",
          "[game][boss-projectiles][yeti][assets]") {
    const s32 framesPerSecond = GENERATE(30, 60, 120);
    const f32 dt = 1.0f / static_cast<f32>(framesPerSecond);
    CAPTURE(framesPerSecond);
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELI5/WORLDS.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I5");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    test::CombatantFixture fixture;
    fixture.open(device, root, &world.collision(), {}, 'I');
    REQUIRE(fixture.spawn("YETI", {9.6015625f, -3.5f, -67.203125f}, 0));
    const std::vector<EnemyView> players{EnemyView{0, {6.375f, -3.6484375f, -19.875f}, 1, 6}};
    EffectTrees effects;
    CombatantProjectiles projectiles;
    bool launched = false;
    for (s32 frame = 0; frame < 3600 && !launched; ++frame) {
        fixture.update(2, 1.0f / 30, players);
        for (const auto& shot : fixture.actor.takeShots()) {
            if (shot.damageIndex == 9 || shot.damageIndex == 10) {
                INFO("Launch " << shot.origin.x << ", " << shot.origin.y << ", " << shot.origin.z);
                REQUIRE(shot.endVisual);
                projectiles.launch(shot, fixture.assets.archive, device, effects, {});
                launched = true;
                break;
            }
        }
        fixture.actor.takeCues();
        fixture.actor.takeBlows();
    }
    REQUIRE(launched);
    const Vec3 launchPosition = effects.effect(0).position;
    for (s32 frame = 0; frame < framesPerSecond / 2; ++frame) {
        CAPTURE(frame);
        effects.update(dt);
        projectiles.update(dt, &world.collision(), {}, device, effects, {});
        REQUIRE(projectiles.count() == 1);
        REQUIRE(effects.effect(0).name == "ATTACK8FXB");
    }
    REQUIRE(effects.effect(0).position.z > launchPosition.z + 20);
    // The rebound must still reach the player, not merely leave a stationary effect alive.
    for (s32 frame = 0; frame < framesPerSecond * 2 && projectiles.count() != 0; ++frame) {
        effects.update(dt);
        projectiles.update(dt, &world.collision(), players, device, effects, {});
    }
    const auto hits = projectiles.takeHits();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().player == 0);
    REQUIRE(hits.front().damage == 100);
}

TEST_CASE("Wraith snakes morph and the lantern shortens only their birth effect",
          "[game][boss-projectiles][wraith][assets]") {
    Fixture f;
    const auto root = test::assetOrSkip("CRITTER/WRAITH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/WRAITH/ANIM.PS2");
    REQUIRE(f.data.load(root / "CRITTER/WRAITH.WAD"));
    REQUIRE(f.archive.load(root / "MONSTERS/WRAITH"));
    CombatShot shot;
    shot.data = &f.data;
    shot.damageIndex = 10;
    shot.origin = {0, 2, 0};
    shot.birthLife = 0.25f;
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    REQUIRE(f.effects.effect(0).name == "SNAKEFX");
    REQUIRE(f.effects.effect(0).secondsLeft == Approx(0.25f));
    f.step(0.2f);
    REQUIRE(f.effects.effect(0).name == "SNAKEFX");
    REQUIRE(glm::length(f.effects.effect(0).position - shot.origin) == Approx(10));
    f.step(0.06f);
    REQUIRE(f.effects.effect(0).name == "SNAKELOOP");
    REQUIRE(f.effects.effect(0).secondsLeft == Approx(15));
    const Vec3 at = f.effects.effect(0).position;
    const std::array<EnemyView, 1> players{{{3, {at.x, 0, at.z}, 1, 6}}};
    f.step(1.0f / 30, players);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.projectiles.takeHits().size() == 1);
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0);

    shot.damageIndex = 8;
    shot.birthLife = 0;
    shot.origin = {0, 3, 0};
    shot.target = Vec3{0, 3, 30};
    f.projectiles.launch(shot, f.archive, f.device, f.effects, f.sound);
    REQUIRE(f.effects.effect(0).name == "ATK07LP");
    const std::array<EnemyView, 1> victim{{{8, {0, 0, 1}, 1, 6}}};
    f.step(1.0f / 30, victim);
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.effects.effect(0).name == "ATK07WEB");
    REQUIRE(f.effects.effect(0).secondsLeft == Approx(20));
    const auto hit = f.projectiles.takeHits();
    REQUIRE(hit.size() == 1);
    REQUIRE(hit[0].flags == 0x4000000);
    f.projectiles.clear(f.effects);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("retail boss projectile records retain physics and effect transitions",
          "[game][boss-projectiles][assets]") {
    const auto root = test::assetOrSkip("CRITTER/DRIDER.WAD").parent_path().parent_path();
    for (s32 kind = 34; kind <= 44; ++kind) {
        const std::string name{bossNameOf(kind)};
        DYNAMIC_SECTION(name) {
            const auto wad = test::assetOrSkip("CRITTER/" + name + ".WAD");
            test::assetOrSkip("MONSTERS/" + name + "/ANIM.PS2");
            const auto raw = formats::parseCritterWad(readFile(wad));
            CritterData data;
            REQUIRE(data.load(root / "CRITTER" / (name + ".WAD")));
            REQUIRE(data.damages().size() == raw.damages.size());
            ItemArchive archive;
            REQUIRE(archive.load(root / "MONSTERS" / name));
            test::FakeRenderDevice device;
            EffectTrees effects;
            CombatantProjectiles projectiles;
            usize launched = 0;
            for (usize index = 0; index < data.damages().size(); ++index) {
                const AttackDefinition& damage = data.damages()[index];
                if (damage.type != AttackDefinition::kProjectile || damage.sound < 0) {
                    continue;
                }
                CAPTURE(name, index);
                REQUIRE(damage.behaviorFlags == static_cast<u16>(raw.damages[index].behaviorFlags));
                REQUIRE(damage.gravity == raw.damages[index].gravity);
                REQUIRE(damage.maxSpeed == raw.damages[index].maxSpeed);
                REQUIRE(damage.morph == raw.damages[index].morph);
                REQUIRE(damage.morphEnd == raw.damages[index].morphEnd);
                REQUIRE(damage.morphLife == raw.damages[index].morphLife);
                REQUIRE(damage.yawSpread == raw.damages[index].yawSpread);
                CombatShot shot;
                shot.data = &data;
                shot.damageIndex = static_cast<s32>(index);
                shot.origin = {0, 30, 0};
                shot.target = Vec3{0, 5, 60};
                projectiles.launch(shot, archive, device, effects, {});
                REQUIRE(projectiles.count() == 1);
                REQUIRE(effects.count() >= 1);
                projectiles.update(1.0f / 30, nullptr, {}, device, effects, {});
                effects.update(1.0f / 30);
                projectiles.clear(effects);
                REQUIRE(projectiles.count() == 0);
                ++launched;
            }
            if (name != "LICH") {
                REQUIRE(launched > 0);
            }
        }
    }
}
} // namespace
