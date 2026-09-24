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
#include "formats/CritterWad.h"
#include "game/enemies/BossDefinition.h"
#include "game/enemies/CombatantFixture.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/CombatantProjectiles.h"
#include "game/world/LevelWorld.h"

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
                 "minSpeed":30,"maxSpeed":30,"sfxIndex":5,"sfx":2}],
            "sounds":[{"name":"SHOT","levelFormat":"S_%cSHOT"},{"name":"LOOP"},
                      {"name":"HIT","flags":16,"levelFormat":"S_%cHIT"},{"name":"LOOP","life":100},
                      {"name":"SHOT","flags":131072},{"name":"SHOT","flags":4194304}]})");
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
              const WorldCollision* collision = nullptr) {
        effects.update(seconds);
        projectiles.update(seconds, collision, players, device, effects, sound);
    }
};

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
          "[boss-projectiles][spider][lich][unpacked]") {
    const bool hitPlayer = GENERATE(false, true);
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    for (const std::string name : {"LICH", "DRIDER"}) {
        DYNAMIC_SECTION(name) {
            test::unpackedOrSkip("MONSTERS/" + name + "/animations.json");
            CritterData data;
            REQUIRE(data.load(root / "critter" / (name + ".json")));
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
                usize hits = 0;
                for (s32 frame = 0; frame < 900 && projectiles.count() > 0; ++frame) {
                    effects.update(1.0f / 30);
                    projectiles.update(1.0f / 30, nullptr, players, device, effects, {});
                    for (const auto& hit : projectiles.takeHits()) {
                        ++hits;
                        REQUIRE((hit.flags & PlayerImpact::kSticky) == 0);
                        const PlayerImpact impact{hit.flags, hit.direction};
                        REQUIRE(impact.reaction(hit.damage, 0, false) != PlayerDeed::Webbed);
                    }
                }
                REQUIRE(hits == (hitPlayer ? 1 : 0));
                REQUIRE(projectiles.takeGenerators().size() == 1);
                ++eggs;
            }
            REQUIRE(eggs == (name == "LICH" ? 1 : 2));
            projectiles.clear(effects);
        }
    }
}

TEST_CASE("Garm's two body-break projectiles request summons without player damage",
          "[boss-projectiles][garm][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/GARM.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GARM/animations.json");
    CritterData data;
    REQUIRE(data.load(root / "critter/GARM.json"));
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
        const auto root = test::unpackedOrSkip("PLAYERS/WAR/RED/objects.json")
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
                                            .named = [](std::string_view) {}};
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
          "[boss-projectiles][player-impact][party-motion][spider][unpacked]") {
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
          "[game][boss-projectiles][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/YETI.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/YETI/animations.json");
    Fixture f;
    REQUIRE(f.data.load(root / "critter/YETI.json"));
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
}

TEST_CASE("Yeti mouth-conjured throw survives its launch in the I5 arena",
          "[game][boss-projectiles][yeti][unpacked]") {
    const s32 framesPerSecond = GENERATE(30, 60, 120);
    const f32 dt = 1.0f / static_cast<f32>(framesPerSecond);
    CAPTURE(framesPerSecond);
    const auto root = test::unpackedOrSkip("critter/YETI.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/YETI/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELI5/world.json");
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
          "[game][boss-projectiles][wraith][unpacked]") {
    Fixture f;
    const auto root = test::unpackedOrSkip("critter/WRAITH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/WRAITH/animations.json");
    REQUIRE(f.data.load(root / "critter/WRAITH.json"));
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
          "[game][boss-projectiles][assets][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRIDER.json").parent_path().parent_path();
    for (s32 kind = 34; kind <= 44; ++kind) {
        const std::string name{bossNameOf(kind)};
        DYNAMIC_SECTION(name) {
            const auto wad = test::assetOrSkip("CRITTER/" + name + ".WAD");
            test::unpackedOrSkip("MONSTERS/" + name + "/animations.json");
            const auto raw = formats::parseCritterWad(readFile(wad));
            CritterData data;
            REQUIRE(data.load(root / "critter" / (name + ".json")));
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
