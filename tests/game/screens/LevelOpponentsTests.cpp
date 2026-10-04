#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/combat/Damage.h"
#include "game/enemies/EnemyMind.h"
#include "game/enemies/LegendItems.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/SafeRocks.h"
#include "game/world/TargetAssist.h"
namespace {
using namespace gdl;
using namespace gdl::game;

s32 firstStandingGenerator(const Generators& generators) {
    for (s32 id = 0; static_cast<usize>(id) < generators.count(); ++id) {
        if (generators.standing(id)) {
            return id;
        }
    }
    return -1;
}

void writeMeleeEnemy(const std::filesystem::path& root, s32 kind) {
    const std::string prefix{enemyKind(kind).prefix};
    const auto archive = root / "MONSTERS" / prefix;
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":")" + prefix + R"(1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":2,"rate":30},
                     {"name":"ATTACK1","frames":3,"rate":30},
                     {"name":"ATTACK1R","frames":2,"rate":30},
                     {"name":"ATTACK3","frames":3,"rate":30},
                     {"name":"ATTACK3R","frames":2,"rate":30},
                     {"name":"HIT1","frames":10,"rate":30},
                     {"name":"HIT2","frames":10,"rate":30}]}]})");
}

TEST_CASE("opponent snapshots take block-provoking attacks from each player's active figure",
          "[level-opponents][critter-block][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    players[1].actor.spawn(1, {}, nullptr, {}, 0);
    players[1].figure = PlayerFigure::load(device, root, players[1].actor.save(), false);
    REQUIRE(players[1].figure);
    auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 2);
    CHECK_FALSE(views[0].blockableAttack); // no figure means no active attack
    CHECK_FALSE(views[1].blockableAttack); // READY is not a provoking action
    players[1].figure->animate(0, 2, 1.0f / 30, PlayerDeed::StrongAttack);
    REQUIRE(players[1].figure->animator().action() == PlayerAnimator::Action::StrongThrow);
    views = LevelOpponents::enemyViews(players);
    CHECK(views[0].player == 3);
    CHECK_FALSE(views[0].blockableAttack);
    CHECK(views[1].player == 1);
    CHECK(views[1].blockableAttack);
    players[1].life = PlayerLife::Dying;
    players[1].figure->animate(0, 2, 1.0f / 30, PlayerDeed::Die);
    REQUIRE(players[1].figure->animator().dying());
    views = LevelOpponents::enemyViews(players);
    CHECK(views[1].hidden);
    CHECK_FALSE(views[1].blockableAttack);
}

TEST_CASE("battlefield entrance tower placements keep their stationary archer algorithm",
          "[level-opponents][battlefield-archer][assets]") {
    const auto* name = GENERATE("H1", "H3");
    const auto root =
        test::assetOrSkip("LEVELS/LEVELH1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip(std::string{"LEVELS/LEVEL"} + name + "/WORLDS.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName(name)));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    const Vec3 perch = std::string_view{name} == "H1" ? Vec3{56.875f, 20.125f, 41.5f}
                                                      : Vec3{17.25f, 6.484375f, 58.125f};
    const auto& enemies = opponents.enemies();
    const auto nearby = enemies.within(perch, 2);
    REQUIRE(nearby.size() == 1);
    CHECK(enemies.kindOf(nearby.front()) == 19);
    CHECK(enemies.variantOf(nearby.front()) == kArcherStrength);
    CHECK(enemies.algorithmOf(nearby.front()) == kThrowWay);
    CHECK_FALSE(enemies.bred(nearby.front()));
    opponents.close();
}

TEST_CASE("courtyard grunts approach the entrance player", "[courtyard-grunt][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("A1")));
    REQUIRE(world.startPoint(0));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, world.startPoint(0)->position, 0);
    LevelFixtures fixtures;
    fixtures.bind({device, world, weapons, effects, audio, 1});
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    auto& enemies = opponents.enemies();
    auto views = LevelOpponents::enemyViews(players);
    views[0].level = 99; // the courtyard scenario's knight
    std::vector<s32> entranceGrunts;
    ViewVolume overhead;
    overhead.position = views[0].position + Vec3{0, 80, 0};
    overhead.forward = {0, -1, 0};
    overhead.up = {0, 0, 1};
    // Exercise the real generator, native animation and level collision path. Previously
    // these two algorithm-14 births circled 15/22 units away, despite targeting the player.
    for (s32 frame = 0; frame < 180; ++frame) {
        opponents.watch(overhead, views[0].position);
        opponents.generators().update(2, enemies, views, fixtures.obstacles());
        for (s32 id = 0; id < Enemies::kMost; ++id) {
            if (enemies.alive(id) && enemies.kindOf(id) == kGruntKind && enemies.bred(id) &&
                enemies.algorithmOf(id) == kZigZagWay &&
                glm::distance(enemies.positionOf(id), views[0].position) < 30 &&
                std::ranges::find(entranceGrunts, id) == entranceGrunts.end()) {
                entranceGrunts.push_back(id);
                CHECK(enemies.memoryOf(id).zigZag.side != 0);
            }
        }
        auto obstacles = opponents.generators().enemyObstacles();
        const auto fixturesObstacles = fixtures.obstacles();
        obstacles.insert(obstacles.end(), fixturesObstacles.begin(), fixturesObstacles.end());
        enemies.update(2, 1.0f / 30, views, obstacles);
    }
    REQUIRE(entranceGrunts.size() == 2);
    for (const s32 id : entranceGrunts) {
        INFO("grunt " << id);
        CHECK(enemies.targetOf(id) == 0);
        CHECK(glm::distance(enemies.positionOf(id), views[0].position) < 3.5f);
    }
    const auto blows = enemies.takeBlows();
    CHECK(std::ranges::count_if(blows, [](const EnemyBlow& blow) {
              return blow.player == 0 && blow.kind == kGruntKind && blow.damage > 0;
          }) >= 2);
}

TEST_CASE("enemy views preserve the player's native collision centre independently of height",
          "[level-opponents][multiplayer][enemy-sight]") {
    ClassStats stats;
    stats.height = 10;
    stats.collisionY = 2;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, &stats, {1, 40, 3}, 0);
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 1);
    CHECK(views[0].player == 3);
    CHECK(views[0].position == Vec3{1, 40, 3});
    CHECK(views[0].height == 10);
    REQUIRE(views[0].collisionHeight);
    CHECK(*views[0].collisionHeight == 2);
}

TEST_CASE("an archer's world hit detonates explosive scenery once without player credit",
          "[level-opponents][projectile-impact][world-destruction][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto stage = test::sampleLevel("enemy-world-shot");
    writeTextFile(stage / "world.json", R"({"objects":[
        {"name":"WALL","position":[0,0,0],"next":-1,"child":-1,
         "flags":327682}]})");
    writeTextFile(stage / "collision.json", R"({"objects":[{"object":0,
        "normals":[0,0,-1],"vertices":[-100,-100,12,100,-100,12,0,100,12]}]})");
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef level;
    level.name = "test";
    level.items = "missing";
    REQUIRE(world.load(device, stage, level));
    constexpr s32 kCart = 0;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 25}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 1, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    REQUIRE(
        opponents.enemies().spawn(EnemySpawn{.kind = kGruntKind, .tier = 4, .algorithm = 23}, {}));
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) { FAIL("world hits do not award player credit"); };
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    for (s32 frame = 0; frame < 180 && world.scene().objectVisible(kCart); ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    CHECK_FALSE(world.scene().objectVisible(kCart));
    CHECK_FALSE(world.collision().solid(kCart));
    const auto explosions = world.takeWorldExplosions();
    REQUIRE(explosions.size() == 1);
    CHECK(explosions.front().z < 12);
    CHECK(world.takeWorldExplosions().empty());
    opponents.close();
}

TEST_CASE("Levitation avoids low enemy melee but not tall enemies or disabled protection",
          "[level-opponents][enemy-melee][damage]") {
    const s32 kind = GENERATE(kRatKind, kGruntKind);
    const bool levitating = GENERATE(false, true);
    const auto root = test::scratchDirectory("enemy-melee-low");
    writeMeleeEnemy(root, kind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0, 0, 2}, 0);
    auto& progress = players[0].actor.save().progress();
    progress.health = 1000;
    progress.inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 60);
    progress.inventory.powerups[0].on = levitating;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    // This contact test has no level geometry; do not probe the unloaded world's floor.
    opponents.enemies().open(device, root, nullptr, 1, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kind));
    REQUIRE(opponents.enemies().spawn(EnemySpawn{.kind = kind, .tier = 1, .placed = true}, {}));
    PlayerHealth health;
    PlayerHealth::Events healthEvents;
    healthEvents.sound = [](std::string_view) {};
    healthEvents.cry = [](std::string_view) {};
    healthEvents.named = [](std::string_view, f32) {};
    usize contacts = 0;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize i, f32 amount, HurtKind hurt, bool directed, const PlayerImpact& hit) {
        CHECK(i == 0);
        CHECK(((hit.flags & Damage::kLow) != 0) == (kind == kRatKind));
        CHECK(((hit.flags & PlayerImpact::kKnockBack) != 0) ==
              (kind == kGruntKind && contacts == 7));
        health.hurt(players[i], amount, hurt, directed, false, 1, healthEvents, hit);
        ++contacts;
    };
    for (s32 frame = 0; frame < 300 && contacts < 8; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(contacts == 8);
    if (kind == kRatKind && levitating) {
        CHECK(progress.health == 1000);
        CHECK(players[0].hitFlashTicks == 0);
        CHECK(players[0].painOwed == 0);
    } else {
        CHECK(progress.health < 1000);
    }
    opponents.close();
}

TEST_CASE("Hand of Death and Health Vamp return melee without player pain or kill credit",
          "[level-opponents][enemy-melee][damage]") {
    // GUNE5D 8004DF58: A1E returns physical damage; A20 returns magic damage and
    // calls heal_player (800784E0). Both use player -1 and StartGemFX(col_pos, 1).
    const u32 flags = GENERATE(0x200000U, 0x400000U);
    const bool enabled = GENERATE(false, true);
    const s32 startingHealth = GENERATE(450, 499, 550);
    const f32 damageScale = GENERATE(1.0f, 10.0f);
    const auto root = test::scratchDirectory("enemy-melee-gems");
    writeMeleeEnemy(root, kGruntKind);
    const auto gem = root / "gem";
    std::filesystem::create_directories(gem);
    for (const auto* file : {"objects.json", "textures.json", "body.obj", "skin.png"}) {
        std::filesystem::copy_file(root / "MONSTERS/GRU" / file, gem / file,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    writeTextFile(gem / "animations.json", R"({"trees":[{"name":"GETGEMORANGE",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"FLASH","frames":30,"rate":30}]}]})");
    test::convertModelFixture(gem);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.powerups().load(gem));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, {100, 0, 100}, 0);
    ClassStats stats;
    stats.height = 6;
    stats.collisionY = 4;
    players[1].actor.spawn(2, {}, &stats, {0, 0, 2}, 0);
    auto& progress = players[1].actor.save().progress();
    progress.health = startingHealth;
    progress.inventory.addPowerup(powerup::kSpecial, flags, 0, 60);
    progress.inventory.powerups[0].on = enabled;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    // Test flinching versus magical knockdown, then lethal returns. The world's player
    // level must not scale an uncredited return from a level-one recipient.
    opponents.enemies().open(device, root, nullptr, 2, {.damage = damageScale, .playerLevel = 50},
                             1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    const auto enemy =
        opponents.enemies().spawn(EnemySpawn{.kind = kGruntKind, .tier = 3, .placed = true}, {});
    REQUIRE(enemy);
    const f32 enemyHealth = opponents.enemies().healthOf(*enemy);
    PlayerHealth health;
    PlayerHealth::Events healthEvents;
    healthEvents.sound = [](std::string_view) {};
    healthEvents.cry = [](std::string_view) {};
    healthEvents.named = [](std::string_view, f32) {};
    usize damageEvents = 0;
    usize rewards = 0;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [&](s32, s32, bool) { ++rewards; };
    events.hurt = [&](usize i, f32 amount, HurtKind hurt, bool directed, const PlayerImpact& hit) {
        CHECK(i == 1);
        if (amount > 0) {
            ++damageEvents;
        }
        health.hurt(players[i], amount, hurt, directed, false, 1, healthEvents, hit);
    };
    for (s32 frame = 0;
         frame < 120 && damageEvents == 0 && opponents.enemies().healthOf(*enemy) == enemyHealth;
         ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    CHECK(rewards == 0);
    CHECK(players[0].actor.save().health() == 500);
    if (enabled) {
        CHECK(damageEvents == 0);
        CHECK(opponents.enemies().healthOf(*enemy) ==
              Catch::Approx(enemyHealth - 15 * damageScale));
        const auto healing = static_cast<s32>(15 * damageScale);
        const s32 healed =
            startingHealth < 500 ? std::min(startingHealth + healing, 500) : startingHealth;
        CHECK(progress.health == (flags == 0x400000U ? healed : startingHealth));
        if (damageScale == 1) {
            REQUIRE(opponents.enemies().animatorOf(*enemy) != nullptr);
            CHECK(opponents.enemies().animatorOf(*enemy)->action() ==
                  (flags == 0x400000U ? EnemyAction::HitReact2 : EnemyAction::HitReact1));
        }
        CHECK(players[1].hitFlashTicks == 0);
        CHECK(players[1].painOwed == 0);
        REQUIRE(effects.count() == 1);
        CHECK(effects.effect(0).name == "GETGEMORANGE");
        CHECK(effects.effect(0).position == Vec3{0, 4, 2});
        CHECK(effects.effect(0).archive == &world.powerups());
    } else {
        CHECK(damageEvents == 1);
        CHECK(opponents.enemies().healthOf(*enemy) == enemyHealth);
        CHECK(progress.health < startingHealth);
        CHECK(effects.count() == 0);
    }
    opponents.close();
    CHECK(effects.count() == 0);
}

TEST_CASE("enemy melee plays a dedicated impact on each contact including warded blows",
          "[level-opponents][enemy-melee][assets]") {
    const auto assets = test::assetOrSkip("audio/COMMON.vbk").parent_path().parent_path();
    const bool warded = GENERATE(false, true);
    const s32 tier = GENERATE(1, 3);
    const auto root = test::scratchDirectory("enemy-impact-sound");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    audio.open(assets, &sound, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, {0, 0, 2}, 0);
    auto& progress = players[0].actor.save().progress();
    progress.health = 1000;
    progress.inventory.addPowerup(powerup::kSpecial, powerup::kHandOfDeath, 0, 60);
    progress.inventory.powerups[0].on = warded;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 1, {.health = 100}, 1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    REQUIRE(opponents.enemies().spawn(EnemySpawn{.kind = kGruntKind, .tier = tier, .placed = true},
                                      {}));
    PlayerHealth health;
    PlayerHealth::Events healthEvents;
    healthEvents.sound = [](std::string_view) { FAIL("The impact was already sounded"); };
    healthEvents.cry = [](std::string_view) { FAIL("Melee mode zero must not cry out"); };
    healthEvents.named = [](std::string_view, f32) {};
    usize contacts = 0;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize i, f32 amount, HurtKind hurt, bool directed, const PlayerImpact& hit) {
        CHECK(i == 0);
        CHECK(hurt == HurtKind::QuietBlow);
        ++contacts;
        health.hurt(players[i], amount, hurt, directed, false, 1, healthEvents, hit);
    };
    for (s32 frame = 0; frame < 300 && sound.voiceCount() < 2; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(sound.voiceCount() == 2);
    CHECK(contacts == (warded ? 0 : 2));
    std::array<f32, 8192> samples{};
    mixer.mix(samples);
    CHECK(std::ranges::any_of(samples, [](f32 value) { return value != 0; }));
    opponents.close();
    audio.close();
    mixer.mix(samples); // Drain the stopped streams' fade-out before retiring their voices.
    sound.update();
    CHECK(sound.voiceCount() == 0);
}

TEST_CASE("ordinary aimed shots break Garm's limbs and spawn brood in his actual arena",
          "[level-opponents][garm][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELH4/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GARM/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GRM/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("H4")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    auto& boss = opponents.bosses();
    REQUIRE(boss.present());
    REQUIRE(boss.position());
    boss.wake();
    const Vec3 origin = *boss.position() + Vec3{0, 3, 35};
    const auto targets = boss.targets();
    const auto aim = TargetAssist::select(origin, Vec3{0, 0, -1}, targets, 100);
    REQUIRE(aim);
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.position = origin;
    launch.velocity = TargetAssist::velocity(origin, *aim, 60, MissileSpec::of(0).weight);
    launch.spec = &MissileSpec::of(0);
    launch.damage = 1000;
    REQUIRE(missiles.launch(launch));
    for (s32 step = 0; step < 180 && missiles.count() > 0; ++step) {
        missiles.update(1.0f / 60, &world.collision(), targets);
    }
    const auto impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CAPTURE(*aim, impacts[0].target, impacts[0].node);
    REQUIRE(impacts[0].target == Bosses::kTargetId);
    REQUIRE(impacts[0].node >= 0);
    EnemyHit hit;
    hit.damage = impacts[0].damage;
    hit.node = impacts[0].node;
    boss.hurt(hit);
    const usize before = opponents.enemies().count();
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    for (s32 frame = 0; frame < 300 && opponents.enemies().count() == before; ++frame) {
        opponents.update(2, 1.0f / 30, {}, {}, events);
        effects.update(1.0f / 30);
    }
    REQUIRE(opponents.enemies().count() == before + 2);
    opponents.close();
}

TEST_CASE("Chimera arena binds and updates head health meters through the opponent phase",
          "[game][screens][level-opponents][chimera][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("CRITTER/CHIMERA.WAD");
    test::assetOrSkip("MONSTERS/CHIMERA/ANIM.PS2");
    test::assetOrSkip("ITEMS/LEVELA5/objects.ngc");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    REQUIRE(opponents.meter().count() == 3);
    REQUIRE(opponents.meter().showing());
    const auto before = opponents.bosses().healthMeters();
    REQUIRE(before.size() == 3);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    EnemyHit hit;
    hit.damage = 62;
    opponents.bosses().wake();
    opponents.bosses().hurt(hit, 2);
    opponents.update(2, 1.0f / 30.0f, {}, {}, events);
    CHECK(opponents.meter().meter(0).shown() == before[0].health);
    CHECK(opponents.meter().meter(1).shown() == before[1].health - 6);
    CHECK(opponents.meter().meter(2).shown() == before[2].health);
    opponents.close();
    REQUIRE_FALSE(opponents.meter().bound());
}

TEST_CASE("a blast reaches a generator once, unless its blow is slight enough to come again",
          "[level-opponents][generators][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELE/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("E1")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.generators().count() > 0);
    const s32 generator = firstStandingGenerator(opponents.generators());
    REQUIRE(generator >= 0);
    const Vec3 at = opponents.generators().positionOf(generator);
    const f32 full = opponents.generators().healthOf(generator);
    std::vector<s32> reached;
    opponents.blast(at, 1.0f, 2.0f, reached, players);
    const f32 once = opponents.generators().healthOf(generator);
    CHECK(once < full);
    CHECK(reached.empty()); // two or less may come again
    opponents.blast(at, 1.0f, 2.0f, reached, players);
    CHECK(opponents.generators().healthOf(generator) < once);
    const f32 twice = opponents.generators().healthOf(generator);
    opponents.blast(at, 1.0f, 3.0f, reached, players);
    CHECK(reached == std::vector<s32>{1000 + generator});
    const f32 thrice = opponents.generators().healthOf(generator);
    CHECK(thrice < twice);
    opponents.blast(at, 1.0f, 3.0f, reached, players);
    CHECK(opponents.generators().healthOf(generator) == thrice);
    opponents.close();
}

TEST_CASE("Temple generator damage plays realm particles and each accepted hit sounds",
          "[level-opponents][generators][enemy-feedback][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELE/ANIM.PS2");
    test::assetOrSkip("AUDIO/CATH.VBK");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("E1")));
    ItemArchive weapons;
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    audio.open(root, &sound, world.audio(), 'E');
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.generators().count() > 0);
    const s32 generator = firstStandingGenerator(opponents.generators());
    REQUIRE(generator >= 0);
    REQUIRE(opponents.generators().kindOf(generator) == -2);
    const f32 health = opponents.generators().healthOf(generator);
    opponents.strikeGenerator(generator, 1, 0);
    CHECK(opponents.generators().healthOf(generator) < health);
    CHECK(effects.count() == 0); // audio on damage, debris only when the state crumbles
    CHECK(sound.voiceCount() == 1);
    opponents.strikeGenerator(generator, health * 0.5f, 0);
    REQUIRE(effects.count() == 1);
    CHECK(effects.effect(0).name == "GENHIT");
    CHECK(effects.effect(0).archive == &world.items());
    CHECK(effects.effect(0).particles.field().size() == 3);
    CHECK(sound.voiceCount() == 2);
    effects.update(0.2f);
    const auto& effect = effects.effect(0);
    CHECK(effect.particles.field().particleCount() > 0);
    for (usize i = 0; i < effect.particles.field().size(); ++i) {
        CHECK(effect.particles.field().textureOf(i) != &device.whiteTexture());
        CHECK(effect.particles.field().emitter(i).node() ==
              effect.transform() * effect.pose.matrices()[i + 2]);
    }
    opponents.strikeGenerator(generator, health, 0);
    REQUIRE(effects.count() == 2);
    CHECK(effects.effect(1).name == "GENDIE");
    CHECK(effects.effect(1).particles.field().size() == 4);
    CHECK(sound.voiceCount() == 3);
    opponents.strikeGenerator(generator, health, 0);
    CHECK(sound.voiceCount() == 3);

    // Follow the real hit -> feedback -> bank -> mixer path, not just the name builder.
    auto& enemies = opponents.enemies();
    REQUIRE(enemies.loadKind(13));
    const auto enemy = enemies.spawn(EnemySpawn{.kind = 13, .tier = 2, .placed = true}, {});
    REQUIRE(enemy);
    LevelOpponents::Events events;
    events.award = [](s32, s32, bool) {};
    events.levels = [] {};
    EnemyHit hit;
    hit.damage = 1;
    enemies.hurt(*enemy, hit);
    opponents.settleRewards(players, events);
    CHECK(sound.voiceCount() == 4);
    hit.damage = 100000;
    enemies.hurt(*enemy, hit);
    opponents.settleRewards(players, events);
    CHECK(sound.voiceCount() == 5);
    opponents.settleRewards(players, events);
    CHECK(sound.voiceCount() == 5);
    std::array<f32, 8192> samples{};
    mixer.mix(samples);
    CHECK(std::ranges::any_of(samples, [](f32 value) { return value != 0; }));
    opponents.close();
    CHECK(effects.count() == 0);
    audio.close();
}

TEST_CASE("a damaged gargoyle's authored roar reaches the battlefield sound bank and mixer",
          "[level-opponents][gargoyle-roar][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GAR_EAGL.WAD").parent_path().parent_path();
    test::assetOrSkip("audio/BATTLE.vbk");
    const auto stage = test::sampleLevel("gargoyle-roar-audio");
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef level;
    level.name = "test";
    level.items = "missing";
    REQUIRE(world.load(device, stage, level));
    ItemArchive weapons;
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    LevelAudioInfo bank;
    bank.bank = "BATTLE";
    audio.open(root, &sound, &bank, 'H');
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 20}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    auto& critters = opponents.critters();
    critters.open(device, root, nullptr, {}, 'H');
    const auto id = critters.spawnGargoyle(Vec3{0}, 0, "GAR_EAGL");
    REQUIRE(id);
    critters.hold(*id, true);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    for (s32 frame = 0; frame < 120; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(sound.voiceCount() == 0);
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 100;
    critters.hurt(*id, hit);
    // Discard the immediate impact cue so only the move's roar reaches this mixer.
    const auto impact = critters.takeCues();
    REQUIRE(impact.size() == 1);
    REQUIRE(impact.front().sound == "S_GRGHHIT");
    for (s32 frame = 0; frame < 90; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    CHECK(sound.voiceCount() == 1);
    std::array<f32, 8192> samples{};
    mixer.mix(samples);
    CHECK(std::ranges::any_of(samples, [](f32 value) { return value != 0; }));
    opponents.close();
    audio.close();
}

TEST_CASE("boss arenas propagate elemental scaling to summoned swarm enemies",
          "[level-opponents][damage][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level()->bossType >= 0);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    auto& enemies = opponents.enemies();
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn(EnemySpawn{.kind = kGruntKind, .tier = 3, .placed = true}, {});
    REQUIRE(id);
    const f32 before = enemies.healthOf(*id);
    EnemyHit hit;
    hit.damage = 20;
    hit.flags = 1;
    enemies.hurt(*id, hit);
    CHECK(enemies.healthOf(*id) ==
          Catch::Approx(before - (20 - enemyKind(kGruntKind).armor) * 1.25f));
    opponents.close();
    effects.clear();
}

TEST_CASE("standing generators block player movement and release it when destroyed",
          "[level-opponents][generator-collision]") {
    const auto root = test::scratchDirectory("player-generator-collision");
    writeMeleeEnemy(root, kGruntKind);
    writeTextFile(root / "MONSTERS/GRU/objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1},
      {"index":1,"name":"GEN_GRU1L1","file":"body.obj","meshTriangles":1}]})");
    test::convertModelFixture(root / "MONSTERS/GRU");
    writeTextFile(root / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
        "xSize":3,"zSize":1,"hitPoints":10}],
      "itemInstances":[{"info":0,"position":[0,0,0],"rotation":[0,0.7,0],
        "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    test::FakeRenderDevice device;
    LevelOpponents opponents;
    WorldLayout layout;
    REQUIRE(layout.load(root));
    opponents.enemies().open(device, root, nullptr, 4, {}, 1);
    REQUIRE(opponents.generators().bind(device, layout, opponents.enemies(), nullptr, {}, 1));
    REQUIRE(opponents.generators().count() == 1);
    const Obstacle box = opponents.generators().boxOf(0);
    PlayerActor player;
    const Vec3 from{0, 0, -12};
    const Vec3 to{0, 0, 12};
    player.spawn(0, {}, nullptr, from, 0);
    Vec3 stopped = from;
    for (s32 step = 0; step < 40; ++step) {
        stopped = opponents.resolveMovement(player, stopped, stopped + Vec3{0, 0, 0.25f});
        CHECK(glm::distance(box.pushOut(stopped, player.radius()), stopped) < 1e-4f);
    }
    CHECK(box.touchedBy(stopped, player.radius()));
    const Vec3 swept = opponents.resolveMovement(player, from, to);
    CHECK(glm::distance(swept, to) > player.radius()); // The rotated box deflects the step.
    CHECK(glm::distance(box.pushOut(swept, player.radius()), swept) < 1e-4f);
    const Vec3 above{0, box.height + 1, -12};
    CHECK(opponents.resolveMovement(player, above, above + Vec3{0, 0, 24}) ==
          above + Vec3{0, 0, 24});
    opponents.generators().strike(0, 1000000, 0);
    REQUIRE_FALSE(opponents.generators().standing(0));
    CHECK(opponents.resolveMovement(player, from, to) == to);
    opponents.close();
}

TEST_CASE("the opponent movement recheck preserves player wall clearance",
          "[level-opponents][collision][tower-wings]") {
    const auto stage = test::sampleLevel("player-wall-clearance");
    writeTextFile(stage / "world.json", R"({"objects":[
      {"name":"GROUND","position":[0,0,0],"flags":4},
      {"name":"LOWERED_GATE","position":[0,0,0],"flags":2},
      {"name":"WALL","position":[0,0,0],"flags":2}]})");
    writeTextFile(stage / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-10,0,-10,10,0,-10,10,0,10,-10,0,-10,10,0,10,-10,0,10]},
      {"object":1,"normals":[-1,0,0,-1,0,0],
       "vertices":[0,-8,-5,0,1.3,-5,0,1.3,5,0,-8,-5,0,1.3,5,0,-8,5]},
      {"object":2,"normals":[-1,0,0,-1,0,0],
       "vertices":[4,0,-5,4,5,-5,4,5,5,4,0,-5,4,5,5,4,0,5]}]})");
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef level;
    level.name = "test";
    level.items = "missing";
    REQUIRE(world.load(device, stage, level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    PlayerActor& actor = players[0].actor;
    actor.spawn(0, {}, nullptr, {-2, 0, 0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, stage, 1}, players);
    // Each walking step clears a lowered gate. A post-body recheck must not
    // move it back to the other side using a different vertical collision span.
    for (s32 step = 0; step < 80; ++step) {
        const Vec3 before = actor.position();
        actor.slide({0.05f, 0, 0}, &world.collision());
        const Vec3 walked = actor.position();
        const Vec3 rechecked = opponents.resolveMovement(actor, before, walked);
        REQUIRE(glm::distance(walked, rechecked) < 1e-4f);
        actor.place(rechecked);
    }
    CHECK(actor.position().x == Catch::Approx(2.0f).margin(0.001f));
    // This is not a bypass of the second wall check: an opponent could push
    // a body into a full-height wall, and that position must still be corrected.
    const Vec3 blocked = opponents.resolveMovement(actor, actor.position(), {3.8f, 0, 0});
    CHECK(blocked.x == Catch::Approx(4.0f - actor.radius()));
    opponents.close();
}

TEST_CASE("mountain creatures stop a player in melee range and release collision on death",
          "[level-opponents][collision][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GOLEM/LEVELB/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelOpponents opponents;
    for (const auto kind : {CombatantKind::Golem, CombatantKind::Gargoyle}) {
        opponents.critters().open(device, root, nullptr, {}, 'B');
        const auto id = opponents.critters().spawn(kind, Vec3{0}, 0.8f);
        REQUIRE(id);
        const auto bodies = opponents.critters().targets(true);
        REQUIRE_FALSE(bodies.empty());
        const Vec3 centre = bodies.back().base + Vec3{0, bodies.back().height * 0.5f, 0};
        const Vec3 from{centre.x, 0, centre.z - 30};
        const Vec3 to{centre.x, 0, centre.z + 30};
        PlayerActor player;
        player.spawn(0, {}, nullptr, from, 0);
        Vec3 stop = from;
        bool contacted = false;
        for (s32 step = 0; step < 240 && !contacted; ++step) {
            const Vec3 wanted = stop + Vec3{0, 0, 0.25f};
            stop = opponents.resolveMovement(player, stop, wanted);
            contacted = glm::distance(stop, wanted) > 1e-4f;
        }
        CAPTURE(static_cast<s32>(kind), stop.x, stop.y, stop.z);
        REQUIRE(contacted);
        const auto target = TargetAssist::around(
            stop, player.height(), opponents.critters().targets(), player.radius() + 1);
        REQUIRE(target);
        CHECK(target->id == *id);
        EnemyHit hit;
        hit.damage = 1000000;
        opponents.critters().hurt(*id, hit);
        REQUIRE_FALSE(opponents.critters().alive(*id));
        CHECK(glm::distance(opponents.resolveMovement(player, from, to), to) < 1e-4f);
        opponents.close();
    }
}

TEST_CASE("the town's IT stands for a party of three, with no body of its own",
          "[level-opponents][it][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G3");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto countIt = [](const LevelOpponents& opponents) {
        s32 found = 0;
        for (s32 id = 0; id < Enemies::kMost; ++id) {
            if (opponents.enemies().alive(id) && opponents.enemies().kindOf(id) == kItKind) {
                ++found;
            }
        }
        return found;
    };
    for (const usize party : {usize{1}, usize{3}}) {
        CAPTURE(party);
        ItemArchive weapons;
        EffectTrees effects;
        LevelSoundscape audio;
        std::vector<PlayerRuntime> players(party);
        for (usize i = 0; i < party; ++i) {
            players[i].actor.spawn(static_cast<s32>(i), {}, nullptr, Vec3{0, 0, 0}, 0);
        }
        LevelOpponents opponents;
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        CHECK(countIt(opponents) == (party >= 3 ? 1 : 0));
        opponents.close();
    }
}

TEST_CASE("Trenches placed archers keep firing after their first arrow",
          "[level-opponents][battle-archer][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELH1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("H1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    Enemies& enemies = opponents.enemies();
    s32 archer = -1;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id) && enemies.variantOf(id) == kArcherStrength &&
            enemies.algorithmOf(id) == kThrowWay) {
            archer = id;
            break;
        }
    }
    REQUIRE(archer >= 0);
    EnemyView player;
    player.player = 0;
    player.position = enemies.positionOf(archer) + Vec3{0, 0, 25};
    player.radius = 1;
    player.height = 6;
    const std::array party{player};
    EnemyMissiles missiles;
    s32 releases = 0;
    for (s32 frame = 0; frame < 180; ++frame) {
        enemies.update(2, 1.0f / 30, party, {}, &missiles);
        if (enemies.animatorOf(archer)->threw()) {
            ++releases;
        }
    }
    CHECK(releases >= 2);
    usize arrows = 0;
    for (usize i = 0; i < missiles.count(); ++i) {
        arrows += missiles.missile(i).shooter == archer ? 1U : 0U;
    }
    CHECK(arrows >= 2);
    opponents.close();
}

TEST_CASE("the placed enemies stand only once the camera comes to see them",
          "[level-opponents][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {24.375f, 0.0078125f, 2.5f}, 0);
    LevelOpponents::Resources resources{device, world, weapons, effects, audio, root, 1};
    resources.standOnSight = true;
    opponents.open(resources, players);
    CHECK(opponents.enemies().count() == 0);
    const usize waiting = opponents.pendingPlacements();
    REQUIRE(waiting > 0);
    // A camera looking at nothing stands nothing.
    ViewVolume nowhere;
    nowhere.position = Vec3{0.0f, -1000.0f, 0.0f};
    nowhere.forward = Vec3{0.0f, -1.0f, 0.0f};
    opponents.watch(nowhere, nowhere.position);
    CHECK(opponents.pendingPlacements() == waiting);
    // Over a one-player grunt, looking down from close by: it stands, and the rest wait.
    // The first placement is Death, whose native archive makes it a touch-woken statue.
    const std::vector<ItemInfo>& infos = world.layout().itemInfos();
    std::optional<Vec3> spot;
    for (const ItemInstance& instance : world.layout().itemInstances()) {
        if (instance.info >= 0 &&
            infos[static_cast<usize>(instance.info)].type == ItemInfo::kPlacedEnemy &&
            infos[static_cast<usize>(instance.info)].name == "GRU" && instance.minPlayers == 1) {
            spot = instance.position;
            break;
        }
    }
    REQUIRE(spot.has_value());
    ViewVolume overhead;
    overhead.position = *spot + Vec3{0.0f, 10.0f, 0.0f};
    overhead.forward = Vec3{0.0f, -1.0f, 0.0f};
    overhead.up = Vec3{0.0f, 0.0f, 1.0f};
    opponents.watch(overhead, *spot);
    CHECK(opponents.pendingPlacements() < waiting);
    CHECK(opponents.pendingPlacements() > 0);
    CHECK(opponents.enemies().count() + opponents.critters().count() > 0);
    opponents.close();
}

TEST_CASE("the tenth hit on what generators bred teaches to destroy generators",
          "[game][screens][level-opponents][help]") {
    const auto root = test::scratchDirectory("hit-streak");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, {0, 0, 20}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 2, {.health = 1000}, 1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    const auto bred = opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .generator = 0, .placed = true}, {});
    const auto placed = opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .position = {5, 0, 0}, .placed = true}, {});
    REQUIRE(bred.has_value());
    REQUIRE(placed.has_value());
    CHECK(opponents.enemies().bred(*bred));
    CHECK_FALSE(opponents.enemies().bred(*placed));
    std::vector<s32> helps;
    LevelOpponents::Events events;
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.help = [&](s32 id, usize player) {
        CHECK(player == 0);
        helps.push_back(id);
        return true;
    };
    const auto hit = [&](s32 id) {
        opponents.strikeEnemy(id, 5.0f, 0, {0, 0, 1}, 2, players);
        opponents.settleRewards(players, events);
    };
    for (s32 i = 0; i < 20; ++i) {
        hit(*placed); // what a level placed does not count
    }
    for (s32 i = 0; i < 9; ++i) {
        hit(*bred);
    }
    CHECK(helps.empty());
    hit(*bred);
    CHECK(helps == std::vector<s32>{HelpMessages::kDestroyGenerators});
    opponents.close();
}

TEST_CASE("the castle's golem and gargoyle stand as statues until walked into, struck or woken "
          "by the pad at the gargoyle's feet, then come alive where they stood",
          "[level-opponents][critter-statues][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GOLEM/LEVELA/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("A1")));
    world.setPlayerCount(1);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    const Vec3 golem{112.75f, 26.125f, 90.25f};
    const Vec3 gargoyle{-74.9765625f, 0.1015625f, 32.671875f};
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, golem + Vec3{20.0f, 0.0f, 0.0f}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    // For one player, the level's golem and gargoyle: neither stands as itself yet (its
    // generals, which have no statue, do).
    const auto standing = [&](CombatantKind kind) {
        std::optional<s32> found;
        for (s32 id = 0; id < Critters::kMost; ++id) {
            if (opponents.critters().alive(id) && opponents.critters().kindOf(id) == kind) {
                found = id;
            }
        }
        return found;
    };
    // The third item is the red Death's sleeping statue (placement strength 0).
    REQUIRE(opponents.statues().count() == 3);
    CHECK_FALSE(standing(CombatantKind::Golem).has_value());
    CHECK_FALSE(standing(CombatantKind::Gargoyle).has_value());
    const usize generals = opponents.critters().count();
    const auto statueNear = [&](const Vec3& at) {
        for (usize i = 0; i < opponents.statues().count(); ++i) {
            if (glm::distance(opponents.statues().positionOf(i), at) < 2.0f) {
                return std::optional<usize>{i};
            }
        }
        return std::optional<usize>{};
    };
    REQUIRE(statueNear(golem).has_value());
    REQUIRE(statueNear(gargoyle).has_value());
    CHECK(opponents.statues().placement(*statueNear(golem)).kind == CombatantKind::Golem);
    CHECK(opponents.statues().placement(*statueNear(gargoyle)).kind == CombatantKind::Gargoyle);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    const auto step = [&] { opponents.update(2, 1.0f / 30, players, {}, events); };
    // Standing well off, nothing changes. Walking into the golem's statue (its record's
    // radius of four) stops the player at it and wakes it.
    step();
    CHECK_FALSE(opponents.statues().woken(*statueNear(golem)));
    players[0].actor.place(golem + Vec3{4.5f, 0.0f, 0.0f});
    step();
    CHECK(opponents.statues().woken(*statueNear(golem)));
    CHECK(players[0].actor.position().x > golem.x + 4.5f);
    // Its ACTIVE sequence (fifteen frames at fifteen a second) plays out, and the golem
    // stands in its place, facing as the statue was placed.
    s32 rose = 0;
    for (s32 frame = 0; frame < 40 && statueNear(golem).has_value(); ++frame) {
        step();
        ++rose;
    }
    REQUIRE(opponents.statues().count() == 2);
    CHECK(rose == 30); // sixty ticks, two a step
    REQUIRE(opponents.critters().count() == generals + 1);
    const auto risen = standing(CombatantKind::Golem);
    REQUIRE(risen.has_value());
    CHECK(glm::distance(opponents.critters().positionOf(*risen), golem) < 2.0f);
    CHECK(opponents.critters().moveOf(*risen) == "START");
    // The pad at the gargoyle's feet (flagged 0x2002, chained after the pad thirty units
    // south of it) wakes it as a player steps on the first; the eighty-five frames of its
    // ACTIVE sequence later the gargoyle stands.
    players[0].actor.place(gargoyle + Vec3{40.0f, 0.0f, 0.0f});
    step();
    CHECK_FALSE(opponents.statues().woken(*statueNear(gargoyle)));
    players[0].actor.place(Vec3{-83.75f, 0.2578125f, 3.796875f});
    REQUIRE(glm::distance(players[0].actor.position(), gargoyle) > 20.0f);
    const std::array pad{TriggerVisitor{.position = players[0].actor.position(),
                                        .radius = players[0].actor.radius()}};
    world.updateTriggers(1.0f / 30, pad);
    step();
    CHECK(opponents.statues().woken(*statueNear(gargoyle)));
    for (s32 frame = 0; frame < 120 && statueNear(gargoyle).has_value(); ++frame) {
        step();
    }
    REQUIRE(opponents.statues().count() == 1);
    CHECK(opponents.statues().placement(0).enemy.has_value());
    CHECK_FALSE(opponents.statues().woken(0));
    CHECK(standing(CombatantKind::Gargoyle).has_value());
    // A blow wakes one too.
    opponents.close();
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.statues().count() == 3);
    opponents.wakeStatue(*statueNear(gargoyle));
    CHECK(opponents.statues().woken(*statueNear(gargoyle)));
    CHECK_FALSE(opponents.statues().woken(*statueNear(golem)));
    opponents.close();
}

TEST_CASE("a general carries the pickup it stands on and lets it go when slain",
          "[level-opponents][carried][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GENERAL/LEVELG/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    world.setPlayerCount(4);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {24.375f, 0.0078125f, 2.5f}, 0);
    // G1 stands a general on a banana (items.c fn_8005D0C4: within two along, three up).
    const Vec3 post{5.6015625f, 9.8984375f, -112.3515625f};
    const PlacedItems& items = world.placedItems();
    std::optional<usize> held;
    for (usize i = 0; i < items.size(); ++i) {
        const Vec3 at = items.item(i).position;
        if (items.item(i).name == "BANANNA" && std::hypot(at.x - post.x, at.z - post.z) < 2.0f) {
            held = i;
        }
    }
    REQUIRE(held.has_value());
    REQUIRE(items.item(*held).visible);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    CHECK(items.item(*held).carried);
    CHECK_FALSE(items.item(*held).visible);
    std::optional<s32> general;
    for (s32 id = 0; id < Critters::kMost; ++id) {
        if (opponents.critters().alive(id) &&
            glm::distance(opponents.critters().positionOf(id), post) < 3.0f) {
            general = id;
        }
    }
    REQUIRE(general.has_value());
    std::vector<s32> helps;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 1000000;
    opponents.critters().hurt(*general, hit);
    opponents.settleRewards(players, events);
    CHECK(items.item(*held).carried);
    CHECK_FALSE(items.item(*held).visible);
    bool sawThrow = false;
    bool sawLanding = false;
    Vec3 dropSpot = post;
    f32 highest = post.y;
    for (s32 frame = 0; frame < 180; ++frame) {
        world.update(1.0f / 30);
        effects.update(1.0f / 30);
        opponents.update(2, 1.0f / 30, players, {}, events);
        opponents.settleRewards(players, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "BAG_THROW" || effect.name == "BAG_HIT") {
                CHECK_FALSE(items.item(*held).visible);
                CHECK_FALSE(items.item(*held).takeable());
                highest = std::max(highest, effect.position.y);
                dropSpot = effect.position;
                sawThrow |= effect.name == "BAG_THROW";
                sawLanding |= effect.name == "BAG_HIT";
            }
        }
    }
    CHECK(sawThrow);
    CHECK(sawLanding);
    CHECK(highest > post.y + 2);
    CHECK_FALSE(items.item(*held).carried);
    CHECK(items.item(*held).visible);
    CHECK(helps == std::vector<s32>{HelpMessages::kGeneralsCarry});
    CHECK(items.item(*held).takeable());
    CHECK(items.item(*held).position.x == Catch::Approx(dropSpot.x));
    CHECK(items.item(*held).position.z == Catch::Approx(dropSpot.z));
    opponents.close();
}

TEST_CASE("B1 gargoyle wakes on actual player approach before physical contact",
          "[level-opponents][critter-statues][gargoyle-approach][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELB1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GAR_EAGL/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("B1")));
    world.setPlayerCount(1);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{1000}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    std::optional<usize> gargoyle;
    for (usize i = 0; i < opponents.statues().count(); ++i) {
        if (opponents.statues().placement(i).kind == CombatantKind::Gargoyle) {
            gargoyle = i;
            break;
        }
    }
    REQUIRE(gargoyle);
    const auto& placement = opponents.statues().placement(*gargoyle);
    REQUIRE(placement.sight == 20);
    REQUIRE(placement.radius == 8);
    const Vec3 approached = opponents.statues().positionOf(*gargoyle) + Vec3{0, 0, 15};
    REQUIRE_FALSE(opponents.statues().woken(*gargoyle));
    players[0].actor.place(approached);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    opponents.update(2, 1.0f / 30, players, {}, events);
    CHECK(opponents.statues().woken(*gargoyle));
    CHECK(opponents.statues().rising(*gargoyle));
    CHECK(players[0].actor.position() == approached);
}

TEST_CASE("B1 gargoyle reveals its eagle piece only after its sack lands and opens",
          "[level-opponents][carried][gargoyle-loot][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELB1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("B1")));
    world.setPlayerCount(1);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    const Vec3 post{0.6328125f, 98.4296875f, -11.6640625f};
    const Vec3 start = post + Vec3{0, 0, 8};
    const auto floor = world.collision().floorAt(start, 2, 10);
    REQUIRE(floor.has_value());
    REQUIRE(std::abs(floor->y - post.y) < 2);
    players[0].actor.spawn(0, {}, nullptr, start, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    const auto id = opponents.critters().spawn(CombatantKind::Gargoyle, post, 0.239736f);
    REQUIRE(id.has_value());
    const usize originalCount = world.placedItems().size();
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 1000000;
    opponents.critters().hurt(*id, hit);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    bool thrown = false;
    bool landed = false;
    for (s32 frame = 0; frame < 110; ++frame) {
        world.update(1.0f / 30);
        effects.update(1.0f / 30);
        opponents.update(2, 1.0f / 30, players, {}, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "BAG_THROW" || effect.name == "BAG_HIT") {
                REQUIRE(world.placedItems().size() == originalCount);
                device.draws.clear();
                effects.draw(device, Mat4{1}, {});
                REQUIRE_FALSE(device.draws.empty());
                thrown |= effect.name == "BAG_THROW";
                landed |= effect.name == "BAG_HIT";
            }
        }
    }
    REQUIRE(thrown);
    REQUIRE(landed);
    REQUIRE(world.placedItems().size() == originalCount + 1);
    const auto& reward = world.placedItems().item(originalCount);
    REQUIRE(reward.name == "GARGEAGL");
    REQUIRE(reward.takeable());
    const auto landedOn = world.collision().floorAt(reward.position, 2, 10);
    REQUIRE(landedOn.has_value());
    REQUIRE(reward.position.y == Catch::Approx(landedOn->y + PlacedItems::kThrownFloorLift));
    opponents.close();
}

TEST_CASE("Forsaken Province entrance generators breed with the placed enemy roster loaded",
          "[level-opponents][generators][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    test::assetOrSkip("MONSTERS/MAG/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    fixtures.bind({device, world, weapons, effects, audio, 1});
    fixtures.setPlayerCount(1);
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {24.375f, 0.0078125f, 2.5f}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.enemies().count() == static_cast<usize>(world.level()->maxEnemies));
    std::array<bool, 3> special{};
    bool skirmishBomber = false;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!opponents.enemies().alive(id)) {
            continue;
        }
        const s32 variant = opponents.enemies().variantOf(id);
        if (variant >= kArcherStrength && variant <= kSuicideStrength) {
            special[static_cast<usize>(variant - kArcherStrength)] = true;
        }
        skirmishBomber = skirmishBomber || opponents.enemies().algorithmOf(id) == kSkirmishBombWay;
    }
    CHECK(special == std::array<bool, 3>{true, true, true});
    CHECK(skirmishBomber);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    // A camera high over the player: the generators round them are on screen and breed, taking
    // the places of bodies off it.
    ViewVolume overhead;
    overhead.position = players[0].actor.position() + Vec3{0.0f, 80.0f, 0.0f};
    overhead.forward = Vec3{0.0f, -1.0f, 0.0f};
    overhead.up = Vec3{0.0f, 0.0f, 1.0f};
    for (s32 frame = 0; frame < 300; ++frame) {
        opponents.watch(overhead, players[0].actor.position());
        opponents.update(2, 1.0f / 30, players, fixtures.obstacles(), events);
    }
    s32 bred = 0;
    s32 weakBred = 0;
    for (usize g = 0; g < opponents.generators().count(); ++g) {
        const auto id = static_cast<s32>(g);
        bred += opponents.generators().bredOf(id);
        if (opponents.generators().tierOf(id) == 1) {
            weakBred += opponents.generators().bredOf(id);
        }
    }
    CHECK(bred > 0);
    CHECK(weakBred > 0);
    CHECK(opponents.enemies().count() <= static_cast<usize>(world.level()->maxEnemies));
    opponents.close();
    fixtures.clear();
    effects.clear();
}

TEST_CASE("exit settlement credits a last-frame generator kill once without advancing combat",
          "[shop][level-opponents][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    s32 generator = -1;
    for (usize i = 0; i < opponents.generators().count(); ++i) {
        if (opponents.generators().standing(static_cast<s32>(i))) {
            generator = static_cast<s32>(i);
            break;
        }
    }
    REQUIRE(generator >= 0);
    s32 credited = 0;
    // The destroying blow earns five times the bred kind's row (PlayerDamagedItem).
    const s32 worth = generatorExperience(opponents.generators().kindOf(generator), true);
    CHECK(worth > 0);
    s32 destroyed = 0;
    LevelOpponents::Events events;
    events.award = [&](s32 player, s32 amount, bool killed) {
        REQUIRE(player == 3);
        REQUIRE(amount == worth);
        REQUIRE_FALSE(killed); // generators are not enemy kills and do not feed turbo
        ++credited;
    };
    events.destroyedGenerator = [&](s32 player) {
        CHECK(player == 3);
        ++destroyed;
    };
    events.levels = [] {};
    opponents.strikeGenerator(generator, 1000000, 3);
    REQUIRE_FALSE(opponents.generators().standing(generator));
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    REQUIRE(destroyed == 1);
    opponents.strikeGenerator(generator, 1000000, 3);
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    REQUIRE(destroyed == 1);
    opponents.close();
    effects.clear();
}

TEST_CASE("Wraith entrance stops its persistent portal before the emergence effects",
          "[game][screens][level-opponents][wraith][assets]") {
    const auto root = test::assetOrSkip("CRITTER/WRAITH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/WRAITH/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELJ5/WORLDS.PS2");
    test::assetOrSkip("ITEMS/LEVELJ5/objects.ngc");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("J5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 40);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    u32 portal = 0;
    bool emerged = false;
    bool second = false;
    for (s32 frame = 0; frame < 900 && !second; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "INITFX") {
                REQUIRE_FALSE(emerged);
                portal = effect.id;
                REQUIRE(effect.secondsLeft > 1000);
            } else if (effect.name == "GENFX") {
                REQUIRE(portal != 0);
                REQUIRE_FALSE(effects.playing(portal));
                emerged = true;
            } else if (effect.name == "GENFX2") {
                second = true;
            }
        }
        effects.update(1.0f / 30);
    }
    REQUIRE(portal != 0);
    REQUIRE(emerged);
    REQUIRE(second);
    opponents.close();
    REQUIRE(effects.count() == 0);
}

TEST_CASE("Yeti POUND places a single I5 eruption and restores that arena obstacle",
          "[game][screens][level-opponents][yeti][assets]") {
    const auto root = test::assetOrSkip("CRITTER/YETI.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/YETI/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELI5/WORLDS.PS2");
    test::assetOrSkip("ITEMS/LEVELI5/objects.ngc");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, world.layout(), world.items()));
    rocks.setPlayerCount(1);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, rocks.rock(3).position, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 39);
    REQUIRE(opponents.bosses().raisesArenaRocks());
    rocks.hideForEruptions();
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.arenaTargets = [&rocks] { return rocks.arenaTargets(); };
    usize eruptions = 0;
    events.activateArena = [&](const CombatArenaActivation& activation) {
        REQUIRE(activation.index == 3);
        rocks.scheduleActivation(activation.index, activation.delay);
        ++eruptions;
    };
    for (s32 frame = 0; frame < 3600 && eruptions == 0; ++frame) {
        rocks.update(1.0f / 30.0f);
        opponents.update(2, 1.0f / 30.0f, players, rocks.obstacles(), events);
        if (eruptions == 0) {
            effects.update(1.0f / 30.0f);
        }
    }
    INFO("Last move: " << opponents.bosses().moveName());
    REQUIRE(eruptions == 1);
    usize visuals = 0;
    for (usize i = 0; i < effects.count(); ++i) {
        const auto& effect = effects.effect(i);
        if (effect.name == "ATTACK12_S0") {
            REQUIRE(effect.attachment.has_value());
            REQUIRE(glm::length(effect.position - rocks.rock(3).position) < 0.001f);
            ++visuals;
        }
    }
    REQUIRE(visuals == 1);
    REQUIRE(rocks.obstacles().empty());
    rocks.update(34.0f / 30.0f);
    REQUIRE_FALSE(rocks.standing(3));
    rocks.update(1.01f / 30.0f);
    REQUIRE(rocks.standing(3));
    REQUIRE(rocks.obstacles().size() == 1);
    REQUIRE(rocks.rock(3).health == 90);
    // Continue the actual fight with the player behind that freshly raised cover.
    // The scene's typed item stops must reach boss shots, not only swarm missiles.
    players[0].actor.spawn(0, {}, nullptr, rocks.rock(3).position + Vec3{0, 0, 10}, 0);
    events.activateArena = [&](const CombatArenaActivation& activation) {
        rocks.scheduleActivation(activation.index, activation.delay);
    };
    usize iceHits = 0;
    for (s32 frame = 0; frame < 1800 && iceHits == 0; ++frame) {
        rocks.update(1.0f / 30.0f);
        std::vector<MissileStop> stops;
        for (usize i = 0; i < rocks.size(); ++i) {
            if (rocks.standing(i)) {
                const auto& rock = rocks.rock(i);
                stops.push_back({.box = rock.obstacle,
                                 .rock = static_cast<s32>(i),
                                 .rockHealth = rock.health,
                                 .rockArmor = rock.armor});
            }
        }
        opponents.update(2, 1.0f / 30.0f, players, rocks.obstacles(), events, stops);
        for (const RockHit& hit : opponents.takeRockHits()) {
            REQUIRE(rocks.standing(hit.rock));
            const s32 before = rocks.rock(hit.rock).health;
            rocks.strike(hit.rock, hit.damage);
            CHECK(rocks.rock(hit.rock).health < before);
            ++iceHits;
        }
        effects.update(1.0f / 30.0f);
    }
    REQUIRE(iceHits > 0);
    opponents.close();
    CHECK(opponents.takeRockHits().empty());
    for (usize i = 0; i < effects.count(); ++i) {
        INFO("Surviving effect: " << effects.effect(i).name);
        REQUIRE(effects.count() == 0); // effects cannot retain a freed boss archive
    }
}

TEST_CASE("Plague Fiend eruptions are rendered at all three K5 arena anchors",
          "[game][screens][level-opponents][plague][assets]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/PBOSS/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELK5/WORLDS.PS2");
    test::assetOrSkip("ITEMS/LEVELK5/objects.ngc");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("K5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, world.layout(), world.items()));
    rocks.setPlayerCount(1);
    const auto anchors = rocks.attackAnchors();
    REQUIRE(anchors.size() == 3);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 35}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 38);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.arenaAnchors = [&rocks] { return rocks.attackAnchors(); };
    usize eruptions = 0;
    for (s32 frame = 0; frame < 3600 && eruptions == 0; ++frame) {
        opponents.update(2, 1.0f / 30.0f, players, {}, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "ATCK10FX") {
                REQUIRE(effect.attachment.has_value());
                REQUIRE(std::ranges::any_of(anchors, [&](const Mat4& anchor) {
                    return glm::length(Vec3{anchor[3]} - effect.position) < 0.001f &&
                           glm::length(Vec3{anchor[2]} - Vec3{effect.transform()[2]}) < 0.001f;
                }));
                ++eruptions;
            }
        }
        effects.update(1.0f / 30.0f);
    }
    INFO("Last move: " << opponents.bosses().moveName());
    REQUIRE(eruptions == 3);
    opponents.close();
    REQUIRE(effects.count() == 0);
}

TEST_CASE("opponent views preserve player identity and hide fallen participants",
          "[game][screens][level-opponents]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{30, 0, 40}, 0);
    players[1].life = PlayerLife::Dying;
    players[0].actor.save().progress().inventory.addPowerup(9, 4, 0, 30);
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 2);
    REQUIRE(views[0].player == 3);
    REQUIRE(views[0].position == players[0].actor.position());
    REQUIRE(views[0].radius == players[0].actor.radius());
    REQUIRE_FALSE(views[0].hidden);
    REQUIRE(views[0].damageable);
    REQUIRE(views[0].invisible);
    REQUIRE(views[1].player == 1);
    REQUIRE(views[1].hidden);
    REQUIRE_FALSE(views[1].damageable);
    players[0].effectGap = 0.25f;
    players[1].breathGap = 0.25f;
    const auto recentViews = LevelOpponents::enemyViews(players);
    CHECK(recentViews[0].recentlyHit);
    CHECK(recentViews[1].recentlyHit);
    players[1].life = PlayerLife::Standing;
    ComboMove::link(players[0].combo, 0, players[1].combo, 1, 0);
    const auto protectedViews = LevelOpponents::enemyViews(players);
    for (const auto& view : protectedViews) {
        CHECK_FALSE(view.hidden); // immunity is not invisibility or untargetability
        CHECK_FALSE(view.damageable);
    }
}

TEST_CASE("opponent phases interleave legend victory and progression in order",
          "[game][screens][level-opponents]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    const auto root = test::scratchDirectory("level-opponents-empty");
    std::vector<std::string> phases;
    const LevelOpponents::Events events{
        .hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) { FAIL("No combatants"); },
        .blast = [](const Vec3&, f32, f32) { FAIL("No combatants"); },
        .settleBlasts = [&] { phases.emplace_back("blast"); },
        .legend = [](const LegendEvent&) { FAIL("No boss"); },
        .advanceLegend =
            [&](f32 seconds) {
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("legend");
            },
        .fallen = [](const Vec3&) { FAIL("No boss"); },
        .spew = [](const CombatSpew&) { FAIL("No boss"); },
        .advanceVictory =
            [&](s32 ticks, f32 seconds) {
                REQUIRE(ticks == 6);
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("victory");
            },
        .levels = [&] { phases.emplace_back("levels"); },
        .award = [](s32, s32, bool) { FAIL("No kills"); },
        .destroyedGenerator = [](s32) { FAIL("No generators"); },
        .blocksBreath = {},
        .blocksArea = {},
        .arenaAnchors = {},
        .arenaTargets = {},
        .activateArena = {},
        .shake = {},
        .help = {},
        .blastScenery = {}};
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases.empty());
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases == std::vector<std::string>{"blast", "legend", "victory", "levels"});
    opponents.close();
    opponents.close();
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases.size() == 4);
    REQUIRE_FALSE(opponents.bosses().present());
    REQUIRE_FALSE(opponents.meter().bound());
}

TEST_CASE("area immunity counts down independently for each player's runtime",
          "[game][screens][level-opponents][boss-areas]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    const auto root = test::scratchDirectory("area-immunity");
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    players[1].actor.spawn(1, {}, nullptr, {}, 0);
    players[0].effectGap = 0.25f;
    players[1].effectGap = 0.05f;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.update(6, 0.1f, players, {}, events);
    REQUIRE(players[0].effectGap > 0.14f);
    REQUIRE(players[0].effectGap < 0.16f);
    REQUIRE(players[1].effectGap == 0);
    opponents.update(12, 0.2f, players, {}, events);
    REQUIRE(players[0].effectGap == 0);
}

TEST_CASE("area contacts share effect immunity but not the breath timer",
          "[game][screens][level-opponents][boss-areas]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0, 0, 12}, 0);
    LevelOpponents::Events events;
    s32 contacts = 0;
    events.hurt = [&](usize index, f32 amount, HurtKind kind, bool directed, const PlayerImpact&) {
        REQUIRE(index == 0);
        REQUIRE(amount == 50);
        REQUIRE(kind == HurtKind::Blow);
        REQUIRE(directed);
        ++contacts;
    };
    CombatBlow blow;
    blow.player = 3;
    blow.damage = 50;
    blow.area = true;
    blow.repeatGap = 0.25f;
    bool blocked = true;
    events.blocksArea = [&](const Vec3&, const Vec3&) { return blocked; };
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 0);
    REQUIRE(players[0].effectGap == 0);
    blocked = false;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 1);
    REQUIRE(players[0].effectGap == 0.25f);
    REQUIRE(players[0].breathGap == 0);
    blow.critter = 7;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 1);
    players[0].effectGap = 0;
    players[0].breathGap = 1;
    blocked = true;
    blow.origin.z = 3; // contacts within ten units do not consult cover
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 2);
    players[0].effectGap = 0;
    players[0].life = PlayerLife::Dying;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 2);
}

TEST_CASE("a great one's blows land every quarter second through the shared hit gap",
          "[game][screens][level-opponents][breath]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    LevelOpponents::Events events;
    s32 contacts = 0;
    events.hurt = [&](usize, f32, HurtKind kind, bool, const PlayerImpact&) {
        REQUIRE(kind == HurtKind::Blow);
        ++contacts;
    };
    CombatBlow blow;
    blow.player = 3;
    blow.damage = 10;
    blow.gated = true;
    CHECK(LevelOpponents::applyCritterBlow(blow, players, events));
    REQUIRE(contacts == 1);
    REQUIRE(players[0].breathGap == 0.25f); // fxhittime, as breath has it
    CHECK_FALSE(LevelOpponents::applyCritterBlow(blow, players, events));
    REQUIRE(contacts == 1);
    players[0].breathGap = 0;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 2);
    // An ungated contact (a ring, once a move) is not held off.
    blow.gated = false;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 3);
}

TEST_CASE("breath contacts share a player's quarter-second gate across creatures",
          "[game][screens][level-opponents][breath]") {
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    players[1].actor.spawn(1, {}, nullptr, {}, 0);
    players[2].actor.spawn(2, {}, nullptr, {}, 0);
    players[2].life = PlayerLife::Dying;
    std::vector<usize> hurt;
    std::vector<HurtKind> kinds;
    LevelOpponents::Events events;
    events.hurt = [&](usize index, f32 damage, HurtKind kind, bool directed,
                      const PlayerImpact& impact) {
        REQUIRE(damage == 40);
        REQUIRE(directed);
        REQUIRE(impact.flags == PlayerImpact::kKnockDown);
        REQUIRE(impact.direction == Vec3(0, 0, -1));
        hurt.push_back(index);
        kinds.push_back(kind);
    };
    CombatBlow fire;
    fire.player = 3;
    fire.damage = 40;
    fire.flags = PlayerImpact::kKnockDown;
    fire.direction = {0, 0, -1};
    fire.breath = true;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt == std::vector<usize>{0});
    REQUIRE(kinds.back() == HurtKind::Burn);
    REQUIRE(players[0].breathGap == 0.25f);
    fire.critter = 7;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 1);
    fire.player = 1;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt == std::vector<usize>{0, 1});
    fire.player = 2;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 2);
    fire.player = 3;
    fire.breath = false;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 3);
    REQUIRE(kinds.back() == HurtKind::Blow);
    REQUIRE(players[0].breathGap == 0.25f);

    // Run the real level phase to expire the gate, without any assets/combatants.
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    opponents.open(
        {device, world, weapons, effects, audio, test::scratchDirectory("breath-contact-level"), 1},
        {});
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    opponents.update(14, 0.24f, players, {}, events);
    fire.breath = true;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 3);
    opponents.update(1, 0.011f, players, {}, events);
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 4);
    REQUIRE(players[0].breathGap == 0.25f);
}

TEST_CASE("breath blocked by arena cover neither damages nor consumes the breath timer",
          "[game][screens][level-opponents][breath]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 20}, 0);
    LevelOpponents::Events events;
    s32 hits = 0;
    events.hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { ++hits; };
    bool blocked = true;
    events.blocksBreath = [&](const Vec3& from, const Vec3& to) {
        REQUIRE(from == Vec3{0, 7, 0});
        REQUIRE(to.z == 20);
        return blocked;
    };
    CombatBlow fire;
    fire.player = 0;
    fire.damage = 40;
    fire.breath = true;
    fire.origin = Vec3{0, 7, 0};
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hits == 0);
    REQUIRE(players[0].breathGap == 0);
    blocked = false;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hits == 1);
    REQUIRE(players[0].breathGap == 0.25f);
}

TEST_CASE("the level keeps boss effects on their animated node or full model root",
          "[game][screens][level-opponents][breath][boss-effects][stop-time][assets]") {
    const auto root = test::assetOrSkip("CRITTER/DRAGON.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/DRAGON/ANIM.PS2");
    s32 kind = 34;
    f32 distance = 25;
    bool rootEffect = false;
    bool timeStopped = false;
    CritterData lichData;
    SECTION("Dragon FIRE rides its animated mouth") {}
    SECTION("Stop Time does not prevent the Dragon from attacking") {
        timeStopped = true;
    }
    SECTION("Lich attack wind-up rides its elevated root") {
        test::assetOrSkip("CRITTER/LICH.WAD");
        test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
        kind = 41;
        distance = 6;
        rootEffect = true;
        REQUIRE(lichData.load(root / "CRITTER/LICH.WAD"));
    }
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, distance}, 0);
    if (timeStopped) {
        players[0].actor.save().progress().inventory.addPowerup(powerup::kSpecial,
                                                                powerup::kStopTime, 0, 60);
    }
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().spawn(kind, Vec3{0}, 0));
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    u32 effectId = 0;
    s32 attachedFrames = 0;
    for (s32 tick = 0; tick < 2400 && attachedFrames < 20; ++tick) {
        opponents.update(1, 1.0f / 60, players, {}, events);
        for (usize e = 0; e < effects.count(); ++e) {
            const auto& effect = effects.effect(e);
            const bool rootAttack = effect.name.starts_with("ATK") &&
                                    std::ranges::any_of(lichData.sounds(), [&](const auto& sound) {
                                        return sound.tree == effect.name && (sound.flags & 1U) != 0;
                                    });
            if (rootEffect ? !rootAttack : effect.name != "FIRE") {
                continue;
            }
            effectId = effect.id;
            REQUIRE(effect.attachment.has_value());
            const auto parent = rootEffect ? opponents.bosses().rootTransform()
                                           : opponents.bosses().nodeTransform("NODE#01");
            REQUIRE(parent.has_value());
            CAPTURE(effect.name, effect.position.x, effect.position.y, effect.position.z,
                    (*parent)[3].x, (*parent)[3].y, (*parent)[3].z, effect.scale);
            REQUIRE(effect.transform() == *parent);
            if (!rootEffect) {
                REQUIRE(effect.particles.field().size() == 2);
            }
            ++attachedFrames;
        }
        effects.update(1.0f / 60);
    }
    REQUIRE(attachedFrames == 20);
    REQUIRE(effects.playing(effectId));
    opponents.close();
    REQUIRE_FALSE(effects.playing(effectId));
}
TEST_CASE("boss relics choose the first eligible controller rather than party storage order",
          "[level-opponents][boss-relics][multiplayer][assets]") {
    // gauntworld.c's level initialization scans controller slots 0..3 and accepts
    // playing/arriving players (states 1/5/3), never dying or waiting-in-tower slots.
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G5");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 2> players;
    CharacterSave save;
    const s32 realm = legendRealmOf(41);
    REQUIRE(save.progress().relics.addLegend(realm));
    players[0].actor.spawn(3, save, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, save, nullptr, Vec3{0}, 0);
    s32 bearer = 3;
    SECTION("both living holders use controller order") {
        bearer = 1;
    }
    SECTION("a dying holder cannot brandish the relic") {
        players[1].life = PlayerLife::Dying;
    }
    SECTION("a holder waiting in the tower cannot brandish the relic") {
        players[1].life = PlayerLife::InTower;
    }
    SECTION("a fallen first stored holder cannot mask a later living holder") {
        players[0].life = PlayerLife::InTower;
        bearer = 1;
    }
    SECTION("a departed holder is not a participant") {
        players[1].departed = true;
    }
    SECTION("an earlier living controller without the relic is skipped") {
        REQUIRE(players[1].actor.save().progress().relics.spendLegend(realm));
    }
    SECTION("no eligible holder leaves the boss rite inactive") {
        players[0].life = PlayerLife::InTower;
        players[1].life = PlayerLife::Dying;
        bearer = -1;
    }
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 41);
    CHECK(opponents.bosses().legend().player() == bearer);
    CHECK(opponents.bosses().legend().stage() ==
          (bearer < 0 ? LegendRite::Stage::None : LegendRite::Stage::Carried));
    // Selecting a bearer does not consume anyone's inventory before the rite starts.
    CHECK(players[0].actor.save().progress().relics.hasLegend(realm));
    opponents.close();
}

TEST_CASE("the Lich's emergence cue hides the arena mound when he wakes",
          "[game][screens][level-opponents][boss-effects][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2");
    test::assetOrSkip("WDATA/TOWN.WAD");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto& objects = world.layout().objects();
    const auto mound = std::ranges::find(objects, "G5BIGDIRT", &WorldObject::name);
    REQUIRE(mound != objects.end());
    const auto index = static_cast<usize>(std::distance(objects.begin(), mound));
    REQUIRE_FALSE(world.scene().moving(index));
    REQUIRE(world.scene().objectVisible(index));
    const usize triangles = world.collision().triangleCount();
    const Mat4 transform = world.scene().worldTransform(index);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    REQUIRE(opponents.bosses().view().kind == 41);
    REQUIRE(opponents.bosses().position() != nullptr);
    players[0].actor.spawn(0, {}, nullptr, *opponents.bosses().position(), 0);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    opponents.update(2, 1.0f / 30, {}, {}, events);
    REQUIRE(world.scene().objectVisible(index)); // asleep: no callback yet
    opponents.update(2, 1.0f / 30, players, {}, events);
    REQUIRE_FALSE(world.scene().objectVisible(index));
    REQUIRE(world.scene().worldTransform(index) == transform);
    REQUIRE(world.collision().triangleCount() == triangles);
    REQUIRE(world.objectAlpha(index) == 1);
    bool gravel = false;
    for (usize i = 0; i < effects.count(); ++i) {
        gravel |= effects.effect(i).name == "GENFX";
    }
    REQUIRE(gravel);
    REQUIRE_FALSE(world.setObjectVisible("MISSING", false));
    opponents.close();
    effects.clear();
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.scene().objectVisible(index));
}
TEST_CASE("Stop Time prevents melee releases without stopping incoming damage or death",
          "[level-opponents][stop-time]") {
    const auto root = test::scratchDirectory("opponents-stop-time");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0, 0, 2}, 0);
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    auto& enemies = opponents.enemies();
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn(EnemySpawn{.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(id);
    usize contacts = 0;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { ++contacts; };
    const auto tick = [&] { opponents.update(2, 1.0f / 30, players, {}, events); };
    for (s32 frame = 0; frame < 90; ++frame) {
        tick();
    }
    CHECK(contacts == 0);
    CHECK(enemies.animatorOf(*id)->player().frame() == 0);
    inventory.powerups[0].on = false;
    for (s32 frame = 0; frame < 90 && contacts == 0; ++frame) {
        tick();
    }
    REQUIRE(contacts > 0);
    inventory.powerups[0].on = true;
    const usize before = contacts;
    const f32 heldFrame = enemies.animatorOf(*id)->player().frame();
    for (s32 frame = 0; frame < 90; ++frame) {
        tick();
    }
    CHECK(contacts == before);
    CHECK(enemies.animatorOf(*id)->player().frame() == heldFrame);
    opponents.strikeEnemy(*id, 1000, 0, Vec3{0, 0, 1}, -1, players);
    for (s32 frame = 0; frame < 120; ++frame) {
        tick();
    }
    CHECK(enemies.count() == 0);
    CHECK(contacts == before);
    opponents.close();
}

TEST_CASE("IT's touch makes its player it, taking it from whoever was, and says so",
          "[level-opponents][it]") {
    const auto root = test::scratchDirectory("opponents-it");
    writeMeleeEnemy(root, kItKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, -80}, 0);
    players[1].actor.spawn(2, {}, nullptr, Vec3{0, 0, 2}, 0); // against IT
    players[0].itTicks = 50;
    const auto views = LevelOpponents::enemyViews(players);
    CHECK(views[0].it);
    CHECK_FALSE(views[1].it);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    auto& enemies = opponents.enemies();
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kItKind));
    REQUIRE(enemies.spawn(EnemySpawn{.kind = kItKind, .tier = 1, .placed = true}, {}));
    std::vector<std::pair<s32, usize>> helps;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) { FAIL("IT is worth nothing"); };
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) { FAIL("IT never strikes"); };
    events.help = [&](s32 id, usize index) {
        helps.emplace_back(id, index);
        return true;
    };
    for (s32 frame = 0; frame < 300 && helps.empty(); ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    CHECK(helps == std::vector<std::pair<s32, usize>>{{HelpMessages::kNowIt, 1}});
    CHECK(players[0].itTicks == 0);
    CHECK(players[1].itTicks == 1);
    opponents.close();
}

TEST_CASE("the swarm is heard at full within twenty of the nearest player, gone past seventy",
          "[game][screens][level-opponents][enemy-sound]") {
    const std::array hearers{Vec3{0, 0, 0}, Vec3{100, 0, 0}};
    CHECK(LevelOpponents::attenuation(Vec3{10, 0, 0}, hearers) == 1.0f);
    CHECK(LevelOpponents::attenuation(Vec3{0, 0, 20}, hearers) == Catch::Approx(1.0f));
    CHECK(LevelOpponents::attenuation(Vec3{0, 0, 45}, hearers) == Catch::Approx(0.5f));
    CHECK(LevelOpponents::attenuation(Vec3{0, 0, 70}, hearers) ==
          Catch::Approx(0.0f).margin(1e-6f));
    CHECK(LevelOpponents::attenuation(Vec3{95, 0, 0}, hearers) == 1.0f); // the nearer one
    CHECK(LevelOpponents::attenuation(Vec3{0, 0, 500}, {}) == 1.0f);     // nobody standing
}

TEST_CASE("a character hits a generator a hundredth softer a level under the place, a tenth "
          "harder a level over",
          "[level-opponents][generators]") {
    CHECK(LevelOpponents::generatorPowerScale(5, 0.0f) == 1.0f); // no level for the place
    CHECK(LevelOpponents::generatorPowerScale(5, 5.0f) == 1.0f);
    CHECK(LevelOpponents::generatorPowerScale(1, 11.0f) == Catch::Approx(0.9f));
    CHECK(LevelOpponents::generatorPowerScale(13, 10.0f) == Catch::Approx(1.3f));
}

TEST_CASE("a suicide struck down goes up in a burning blast that reaches the party and the swarm "
          "once",
          "[level-opponents][suicide]") {
    const auto root = test::scratchDirectory("suicide-blast");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    // Retain SuicideExplosion's radius 6 (80348144), but intentionally include items.
    const s32 rate = GENERATE(30, 60, 120);
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 3}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    players[2].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    players[1].actor.place(Vec3{0, 0, 5.5f + players[1].actor.radius()});
    players[2].actor.place(Vec3{0, 0, 6.1f + players[2].actor.radius()});
    players[0].actor.save().progress().health = 1000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 2, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    const auto bomber = opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .algorithm = kSuicideWay, .placed = true}, {});
    REQUIRE(bomber.has_value());
    const auto neighbour = opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .position = Vec3{2, 0, 0}, .placed = true}, {});
    REQUIRE(neighbour.has_value());
    const f32 before = opponents.enemies().healthOf(*neighbour);
    std::vector<std::pair<HurtKind, u32>> hurts;
    std::array<s32, 3> contacts{};
    std::array<f32, 3> firstDamage{};
    std::array<f32, 3> firstContact{};
    f32 elapsed = 0;
    usize scenerySteps = 0;
    usize pickupSteps = 0;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.blast = [](const Vec3&, f32, f32) { FAIL("The suicide's blast is its own now"); };
    events.blastScenery = [&](const PickupBlastReach& reach, std::vector<s32>& reached) {
        CHECK(reach.radius <= 6.0f);
        CHECK(reach.damage > 0.0f);
        CHECK(reach.flags == 0x421u);
        if (scenerySteps++ == 0) {
            CHECK(reached.empty());
            reached.push_back(42);
        } else {
            CHECK(reached == std::vector<s32>{42});
        }
    };
    events.hurt = [&](usize player, f32 amount, HurtKind kind, bool, const PlayerImpact& hit) {
        CHECK(amount > 0.0f);
        REQUIRE(player < contacts.size());
        if (contacts[player]++ == 0) {
            firstDamage[player] = amount;
            firstContact[player] = elapsed;
        }
        hurts.emplace_back(kind, hit.flags);
    };
    opponents.strikeEnemy(*bomber, 1000.0f, 0, Vec3{0, 0, 1}, 0, players);
    for (s32 frame = 0; frame < rate * 2; ++frame) {
        elapsed = static_cast<f32>(frame) / static_cast<f32>(rate);
        opponents.update(1, 1.0f / static_cast<f32>(rate), players, {}, events);
        pickupSteps += opponents.takePickupBlasts().size();
    }
    // Fire, a knock down and an explosion (0x421), felt once.
    REQUIRE(hurts.size() == 2);
    CHECK(contacts == std::array<s32, 3>{1, 1, 0});
    CHECK(firstContact[1] > 0.5f);
    CHECK(firstDamage[1] < firstDamage[0]);
    CHECK(hurts[0].first == HurtKind::Blow);
    CHECK((hurts[0].second & 0x421u) == 0x421u);
    CHECK(opponents.enemies().healthOf(*neighbour) < before);
    CHECK(opponents.missiles().burstCount() == 0);
    CHECK(scenerySteps > 1);
    CHECK(pickupSteps == scenerySteps);
    opponents.close();
}

TEST_CASE("a detonated suicide stops drawing its body but ordinary enemies retain their fall",
          "[level-opponents][suicide]") {
    const auto root = test::scratchDirectory("suicide-body-removal");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const s32 way = GENERATE(kSuicideWay, kChaseWay);
    const auto id =
        enemies.spawn(EnemySpawn{.kind = kGruntKind, .algorithm = way, .placed = true}, {});
    REQUIRE(id);
    enemies.draw(device, Mat4{1}, WorldLighting{});
    REQUIRE_FALSE(device.draws.empty());
    device.draws.clear();
    EnemyHit hit;
    hit.damage = 1000;
    enemies.hurt(*id, hit);
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty() == (way == kSuicideWay));
    const auto bursts = enemies.takeBursts();
    CHECK(bursts.size() == (way == kSuicideWay ? 1 : 0));
    device.draws.clear();
    enemies.update(2, 1.0f / 30, {});
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty() == (way == kSuicideWay));
    CHECK(enemies.takeBursts().empty());
}

TEST_CASE("a running suicide cries audibly and scatters animated fragments when its fuse ends",
          "[level-opponents][suicide][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/ICE/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("audio/COMMON.vbk");
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    AudioMixer mixer(48000);
    SoundPlayer sound(mixer);
    LevelSoundscape audio;
    audio.open(root, &sound, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 12}, 0);
    players[0].actor.save().progress().health = 1000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 1, {}, 1);
    REQUIRE(opponents.enemies().loadKind(16));
    const auto bomber = opponents.enemies().spawn(
        EnemySpawn{.kind = 16, .tier = kSuicideStrength, .algorithm = 0, .placed = true}, {});
    REQUIRE(bomber);
    opponents.enemies().draw(device, Mat4{1}, WorldLighting{});
    REQUIRE_FALSE(device.draws.empty());
    device.draws.clear();
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    for (s32 frame = 0; frame < 120 && sound.voiceCount() == 0; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(opponents.enemies().alive(*bomber));
    REQUIRE(sound.voiceCount() == 1);
    std::array<f32, 8192> samples{};
    mixer.mix(samples);
    CHECK(std::ranges::any_of(samples, [](f32 value) { return value != 0; }));
    SECTION("detonates on contact or fuse expiry") {}
    SECTION("a lethal player hit detonates it instead of dissolving its body") {
        EnemyHit hit;
        hit.damage = 1000;
        hit.player = 0;
        opponents.enemies().hurt(*bomber, hit);
        opponents.enemies().draw(device, Mat4{1}, WorldLighting{}, nullptr, &weapons);
        CHECK(device.draws.empty());
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    for (s32 frame = 0; frame < 300 && opponents.enemies().alive(*bomber); ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE_FALSE(opponents.enemies().alive(*bomber));
    REQUIRE(sound.voiceCount() >= 2);
    opponents.enemies().draw(device, Mat4{1}, WorldLighting{}, nullptr, &weapons);
    CHECK(device.draws.empty());
    opponents.enemies().drawShadows(device, Mat4{1}, Vec3{0, 10, 20}, WorldLighting{});
    CHECK(device.draws.empty());
    const EffectTrees::Effect* fragments = nullptr;
    for (usize i = 0; i < effects.count(); ++i) {
        if (effects.effect(i).name == "SUICIDEEXP") {
            fragments = &effects.effect(i);
        }
    }
    REQUIRE(fragments != nullptr);
    fragments->model.draw(device, Mat4{1}, fragments->transform(), WorldLighting{},
                          fragments->pose.matrices());
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE_FALSE(device.draws.front().vertices.empty());
    const Vec3 firstVertex = device.draws.front().vertices.front().position;
    device.draws.clear();
    effects.update(0.3f);
    fragments->model.draw(device, Mat4{1}, fragments->transform(), WorldLighting{},
                          fragments->pose.matrices());
    REQUIRE_FALSE(device.draws.empty());
    CHECK(glm::distance(device.draws.front().vertices.front().position, firstVertex) > 0.1f);
    effects.clear();
    opponents.close();
    audio.close();
    mixer.mix(samples);
    sound.update();
}

TEST_CASE("in the town a suicide leaves a poison cloud that turns through its three trees and "
          "gasses the party while it hangs",
          "[level-opponents][suicide][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    const Vec3 ground{24.375f, 0.0078125f, 2.5f};
    players[0].actor.spawn(0, {}, nullptr, ground, 0);
    players[0].actor.save().progress().health = 1000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.enemies().loadKind(13));
    const auto bomber =
        opponents.enemies().spawn(EnemySpawn{.kind = 13,
                                             .tier = 1,
                                             .algorithm = kSuicideWay,
                                             .position = ground + Vec3{2.0f, 0.0f, 0.0f},
                                             .placed = true,
                                             .priority = EnemySpawn::Priority::Visible},
                                  {});
    REQUIRE(bomber.has_value());
    std::vector<HurtKind> hurts;
    LevelOpponents::Events events;
    events.hurt = [&](usize, f32, HurtKind kind, bool, const PlayerImpact&) {
        hurts.push_back(kind);
    };
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    opponents.strikeEnemy(*bomber, 1000.0f, 0, Vec3{0, 0, 1}, 0, players);
    std::vector<std::string> seen;
    for (s32 frame = 0; frame < 150; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
        for (usize e = 0; e < effects.count(); ++e) {
            const std::string& name = effects.effect(e).name;
            if (std::ranges::find(seen, name) == seen.end()) {
                seen.push_back(name);
            }
        }
    }
    for (const std::string_view tree : {"POISONEXP1", "POISONEXP2", "POISONEXP3", "SUICIDEEXP"}) {
        CHECK(std::ranges::find(seen, tree) != seen.end());
    }
    CHECK(std::ranges::find(seen, "EXPLOSION") == seen.end());
    // Gassed every half second while the cloud harms: more than once, never as a blow.
    CHECK(hurts.size() >= 3);
    CHECK(std::ranges::all_of(hurts, [](HurtKind kind) { return kind == HurtKind::Gas; }));
    opponents.close();
    effects.clear();
}

TEST_CASE("a garm brood's corpse bursts where it lay, then its shot flies on at the party",
          "[level-opponents][death-shot][assets]") {
    const Vec3 toward = GENERATE(Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{-0.6f, 0, -0.8f});
    const auto root =
        test::assetOrSkip("MONSTERS/GRM/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, toward * 14.0f, 0);
    players[0].actor.save().progress().health = 1000;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 2, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kGarmBroodKind));
    const auto brood = opponents.enemies().spawn(
        EnemySpawn{.kind = kGarmBroodKind, .tier = 3, .placed = true}, {});
    REQUIRE(brood.has_value());
    std::vector<std::pair<f32, u32>> hurts;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize, f32 amount, HurtKind kind, bool, const PlayerImpact& hit) {
        CHECK(kind == HurtKind::Blow);
        hurts.emplace_back(amount, hit.flags);
    };
    const auto step = [&] {
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
    };
    const auto showing = [&](std::string_view tree) {
        for (usize e = 0; e < effects.count(); ++e) {
            if (effects.effect(e).name == tree) {
                return true;
            }
        }
        return false;
    };
    const auto checkFacing = [&](std::string_view tree) {
        bool found = false;
        for (usize e = 0; e < effects.count(); ++e) {
            if (effects.effect(e).name == tree) {
                found = true;
                CHECK(glm::dot(Vec3{effects.effect(e).transform()[2]}, toward) > 0.99f);
            }
        }
        REQUIRE(found);
    };
    step();
    opponents.strikeEnemy(*brood, 1000.0f, 0, Vec3{0, 0, 1}, 0, players);
    s32 frames = 0;
    while (opponents.enemies().count() > 0 && frames < 300) {
        step();
        ++frames;
    }
    REQUIRE(frames < 300);
    // As the corpse goes its burst is held where it lay and the unseen shot with it.
    REQUIRE(opponents.missiles().count() == 1);
    CHECK(opponents.missiles().missile(0).heldLeft > 0.0f);
    CHECK(showing("DEATHFX1"));
    checkFacing("DEATHFX1");
    CHECK_FALSE(showing("DEATHFX2"));
    const Vec3 lay = opponents.missiles().missile(0).position;
    while (showing("DEATHFX1") && frames < 400) {
        step();
        ++frames;
    }
    REQUIRE(frames < 400);
    // The burst over, the shot is still where it lay; it then flies on toward the party at
    // twenty a second, hurting the one it passes through for fifty with a knock-down, as it
    // reaches them and again a quarter of a second on, still within it.
    REQUIRE(opponents.missiles().count() == 1);
    CHECK(opponents.missiles().missile(0).position == lay);
    CHECK(hurts.empty());
    step();
    CHECK(showing("DEATHFX2"));
    checkFacing("DEATHFX2");
    for (s32 frame = 0; frame < 30; ++frame) {
        step();
    }
    REQUIRE(hurts.size() == 2);
    for (const auto& [amount, flags] : hurts) {
        CHECK(amount == 50.0f);
        CHECK(flags == 0x100020u);
    }
    for (s32 frame = 0; frame < 90; ++frame) {
        step();
    }
    CHECK(opponents.missiles().count() == 0);
    CHECK_FALSE(showing("DEATHFX2"));
    opponents.close();
    effects.clear();
}

TEST_CASE("a shrinker worn shrinks the swarm: their blows land low for half without a power "
          "blow's growth or knock-back, their missiles are thrown small, and hits on them count "
          "double; a boss's arena leaves them whole",
          "[level-opponents][enemy-melee][shrink]") {
    const bool shrinking = GENERATE(false, true);
    const auto root = test::scratchDirectory("enemy-melee-shrink");
    writeMeleeEnemy(root, kGruntKind);
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0, 0, 2}, 0);
    auto& progress = players[0].actor.save().progress();
    progress.health = 1000;
    progress.inventory.addPowerup(powerup::kSpecial, powerup::kEnemyShrink, 0, 60);
    progress.inventory.powerups[0].on = shrinking;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.enemies().open(device, root, nullptr, 1, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kGruntKind));
    const auto id =
        opponents.enemies().spawn(EnemySpawn{.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(id.has_value());
    std::vector<f32> amounts;
    std::vector<u32> flags;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize i, f32 amount, HurtKind, bool, const PlayerImpact& hit) {
        CHECK(i == 0);
        amounts.push_back(amount);
        flags.push_back(hit.flags);
    };
    for (s32 frame = 0; frame < 300 && amounts.size() < 8; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(amounts.size() == 8);
    const f32 scale = shrinking ? 0.667f : 1.0f;
    CHECK(opponents.enemies().shrink() == Catch::Approx(scale));
    CHECK(opponents.critters().count() == 0);
    CHECK(opponents.missiles().shrink() == Catch::Approx(scale));
    // The grunt strikes for its fight, the eighth blow a power blow half as much again with
    // a knock-back; shrunk, every blow is half, low, and no more than the rest.
    const f32 fight = amounts[0] / (shrinking ? 0.5f : 1.0f);
    for (usize i = 0; i < amounts.size(); ++i) {
        CAPTURE(i);
        const bool power = i == 7;
        if (shrinking) {
            CHECK(amounts[i] == Catch::Approx(0.5f * fight));
            CHECK((flags[i] & Damage::kLow) != 0);
            CHECK((flags[i] & PlayerImpact::kKnockBack) == 0);
        } else {
            CHECK(amounts[i] == Catch::Approx(power ? 1.5f * fight : fight));
            CHECK((flags[i] & Damage::kLow) == 0);
            CHECK(((flags[i] & PlayerImpact::kKnockBack) != 0) == power);
        }
    }
    // A hit on it counts double while it is shrunk.
    const f32 before = opponents.enemies().healthOf(*id);
    opponents.strikeEnemy(*id, 4.0f, 0, Vec3{0, 0, -1}, 0, players);
    const f32 through = 4.0f - enemyKind(kGruntKind).armor;
    REQUIRE(through > 1.0f);
    CHECK(before - opponents.enemies().healthOf(*id) ==
          Catch::Approx(shrinking ? 2.0f * through : through));
    opponents.close();
}
TEST_CASE("generator population includes tower-waiting players but excludes quit participants",
          "[level-opponents][multiplayer][generator-presence][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, {10000, 0, 0}, 0);
    players[1].actor.spawn(1, {}, nullptr, {10000, 0, 0}, 0);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    const auto expected = [&](s32 count) {
        return static_cast<usize>(
            std::ranges::count_if(world.layout().itemInstances(), [&](const ItemInstance& item) {
                return world.layout().itemInfos()[static_cast<usize>(item.info)].type ==
                           ItemInfo::kGenerator &&
                       shownToParty(item.minPlayers, count);
            }));
    };
    const usize pair = expected(2);
    const usize solo = expected(1);
    REQUIRE(pair > solo);
    REQUIRE(opponents.generators().obstacles().size() == pair);
    ViewVolume away;
    away.position = {10000, 0, 0};
    opponents.generators().setView(away);
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    players[1].life = PlayerLife::InTower;
    opponents.update(2, 1.0f / 30, players, {}, events);
    CHECK(opponents.generators().obstacles().size() == pair);
    players[1].departed = true;
    opponents.update(2, 1.0f / 30, players, {}, events);
    CHECK(opponents.generators().obstacles().size() == solo);
    opponents.close();
}
} // namespace
