#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/Damage.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/SafeRocks.h"
#include "game/world/TargetAssist.h"
namespace {
using namespace gdl;
using namespace gdl::game;

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
    healthEvents.named = [](std::string_view) {};
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
    healthEvents.named = [](std::string_view) {};
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
          "[level-opponents][enemy-melee][unpacked]") {
    const auto assets =
        test::unpackedOrSkip("audio/COMMON/sounds.json").parent_path().parent_path().parent_path();
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
    healthEvents.named = [](std::string_view) {};
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

TEST_CASE("Chimera arena binds and updates head health meters through the opponent phase",
          "[game][screens][level-opponents][chimera][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("critter/CHIMERA.json");
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
    test::unpackedOrSkip("ITEMS/LEVELA5/objects.json");
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
    opponents.bosses().hurt(hit, 2);
    opponents.update(2, 1.0f / 30.0f, {}, {}, events);
    CHECK(opponents.meter().meter(0).shown() == before[0].health);
    CHECK(opponents.meter().meter(1).shown() == before[1].health - 6);
    CHECK(opponents.meter().meter(2).shown() == before[2].health);
    opponents.close();
    REQUIRE_FALSE(opponents.meter().bound());
}

TEST_CASE("Temple generator damage plays realm particles and each accepted hit sounds",
          "[level-opponents][generators][enemy-feedback][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE/animations.json");
    test::unpackedOrSkip("audio/CATHEDRAL/sounds.json");
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
    REQUIRE(opponents.generators().kindOf(0) == -2);
    const f32 health = opponents.generators().healthOf(0);
    opponents.strikeGenerator(0, 1, 0);
    CHECK(opponents.generators().healthOf(0) < health);
    CHECK(effects.count() == 0); // audio on damage, debris only when the state crumbles
    CHECK(sound.voiceCount() == 1);
    opponents.strikeGenerator(0, health * 0.5f, 0);
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
    opponents.strikeGenerator(0, health, 0);
    REQUIRE(effects.count() == 2);
    CHECK(effects.effect(1).name == "GENDIE");
    CHECK(effects.effect(1).particles.field().size() == 4);
    CHECK(sound.voiceCount() == 3);
    opponents.strikeGenerator(0, health, 0);
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

TEST_CASE("boss arenas propagate elemental scaling to summoned swarm enemies",
          "[level-opponents][damage][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GRU/animations.json");
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

TEST_CASE("mountain creatures stop a player in melee range and release collision on death",
          "[level-opponents][collision][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/GOLEM.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GOLEM/LEVELB/animations.json");
    test::unpackedOrSkip("MONSTERS/GAR_EAGL/animations.json");
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
        const auto target = TargetAssist::melee(
            stop, player.height(), {0, 0, 1}, opponents.critters().targets(), player.radius() + 1);
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

TEST_CASE("Forsaken Province entrance generators breed with the placed enemy roster loaded",
          "[level-opponents][generators][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
    test::unpackedOrSkip("MONSTERS/MAG/animations.json");
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
    for (s32 frame = 0; frame < 300; ++frame) {
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
          "[shop][level-opponents][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
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
    LevelOpponents::Events events;
    events.award = [&](s32 player, s32 amount, bool killed) {
        REQUIRE(player == 3);
        REQUIRE(amount == 0);
        REQUIRE(killed);
        ++credited;
    };
    events.levels = [] {};
    opponents.strikeGenerator(generator, 1000000, 3);
    REQUIRE_FALSE(opponents.generators().standing(generator));
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    opponents.strikeGenerator(generator, 1000000, 3);
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    opponents.close();
    effects.clear();
}

TEST_CASE("Wraith entrance stops its persistent portal before the emergence effects",
          "[game][screens][level-opponents][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/WRAITH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/WRAITH/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELJ5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELJ5/objects.json");
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
          "[game][screens][level-opponents][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/YETI.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/YETI/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELI5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELI5/objects.json");
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
    opponents.close();
    for (usize i = 0; i < effects.count(); ++i) {
        INFO("Surviving effect: " << effects.effect(i).name);
        REQUIRE(effects.count() == 0); // effects cannot retain a freed boss archive
    }
}

TEST_CASE("Plague Fiend eruptions are rendered at all three K5 arena anchors",
          "[game][screens][level-opponents][plague][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/PBOSS.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/PBOSS/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELK5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELK5/objects.json");
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
    REQUIRE(views[0].invisible);
    REQUIRE(views[1].player == 1);
    REQUIRE(views[1].hidden);
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
        .blocksBreath = {},
        .blocksArea = {},
        .arenaAnchors = {},
        .arenaTargets = {},
        .activateArena = {},
        .shake = {}};
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
          "[game][screens][level-opponents][breath][boss-effects][stop-time][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
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
        test::unpackedOrSkip("critter/LICH.json");
        test::unpackedOrSkip("MONSTERS/LICH/animations.json");
        kind = 41;
        distance = 6;
        rootEffect = true;
        REQUIRE(lichData.load(root / "critter/LICH.json"));
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
TEST_CASE("the Lich's emergence cue hides the arena mound when he wakes",
          "[game][screens][level-opponents][boss-effects][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELG5/world.json");
    test::unpackedOrSkip("wdata/TOWN.json");
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
} // namespace
