#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "fixtures/NativeSoundBank.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/players/MagicPerks.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/Chests.h"
#include "game/world/SafeRocks.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    CameraShake shake;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    MultiplayerMode mode = MultiplayerMode::Normal;
    std::array<PlayerRuntime, 1> players;
    PlayerAttacks::Targets targets{opponents, fixtures, {}};
    Fixture() {
        arsenal.bind({device,
                      classes,
                      weapons,
                      world.collision(),
                      effects,
                      audio,
                      nullptr,
                      {},
                      false,
                      false,
                      nullptr,
                      &mode});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer,
                      &shake, &mode});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    }
};

std::filesystem::path turboAssets() {
    const auto root = test::scratchDirectory("turbo-contacts");
    for (const auto* name : {"PLAYERS/WAR/YEL", "PLAYERS/WAR/SFXYEL", "MONSTERS/GRU"}) {
        const auto dir = root / name;
        std::filesystem::create_directories(dir);
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(dir);
    }
    writeTextFile(root / "MONSTERS/GRU/animations.json", R"({"trees":[{"name":"GRU1",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":60,"rate":30}]}]})");
    writeTextFile(root / "PLAYERS/WAR/YEL/animations.json", R"({"trees":[{"name":"WAR_YEL",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[]}]})");
    std::filesystem::create_directories(root / "PLAYERS/WAR/ANIM");
    writeTextFile(root / "PLAYERS/WAR/ANIM/animations.json", R"({"trees":[{"name":"WAR",
      "nodes":[{"name":"BODY","parent":-1,"position":[0,0,0]}],"sequences":[
      {"name":"READY","frames":60,"rate":30},
      {"name":"ATTPWRB","frames":60,"rate":30},
      {"name":"ATTPWRC","frames":60,"rate":30}]}]})");
    writeTextFile(root / "PLAYERS/WAR/SFXYEL/animations.json", R"({"trees":[{"name":"BURST",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"ACTIVE","frames":60,"rate":30}]}]})");
    std::filesystem::create_directories(root / "pdata");
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0,"turboC1":1},"moveEffects":[{"tree":"BURST"}],"moveStrikes":[
      {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50,"effect":0},
      {"type":2,"startFrame":1,"hitRadius":10,"arc":-1,"offset":[0,9,2],
       "speedMin":30,"speedMax":30,"maxTime":6,"amount":70,"flags":64,
       "damageType":1048576}]})");
    return root;
}

TEST_CASE("weapon modes route player hits through health with shared contact immunity",
          "[game][screens][player-attacks][multiplayer-combat]") {
    const auto mode =
        GENERATE(MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt);
    Fixture f;
    f.mode = mode;
    std::array<PlayerRuntime, 2> party;
    party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 4}, 0);
    for (auto& player : party) {
        player.actor.save().progress().health = 1000;
    }
    PlayerHealth health;
    PlayerHealth::Events events{.block = [](f32, f32) {},
                                .sound = [](std::string_view) {},
                                .cry = [](std::string_view) {},
                                .named = [](std::string_view, f32) {},
                                .learnBlock = [] {}};
    s32 hits = 0;
    f.targets.multiplayer = mode;
    f.targets.players = party;
    f.targets.hurt = [&](usize index, f32 damage, HurtKind kind, const PlayerImpact& impact) {
        REQUIRE(index == 1);
        ++hits;
        health.hurt(party[index], damage, kind, true, false, 1, events, impact);
    };
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true, {}};
    MissileLaunch launch;
    launch.spec = &kSpec;
    launch.owner = 3;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 20};
    launch.damage = 12;
    launch.multiplayer = mode;
    REQUIRE(f.arsenal.missiles().launch(launch));
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.3f, party, f.targets);
    CHECK(hits == (mode == MultiplayerMode::Normal ? 0 : 1));
    CHECK(party[0].actor.save().health() == 1000);
    CHECK(party[1].actor.save().health() == (mode == MultiplayerMode::Hurt ? 988 : 1000));
    CHECK(party[1].reaction ==
          (mode == MultiplayerMode::Stun ? PlayerDeed::Reel : PlayerDeed::None));
    // A cooling victim still reflects a later shot before the damage-time gate.
    party[1].actor.save().progress().inventory.addPowerup(powerup::kArmor, powerup::kReflectShield,
                                                          0, 1);
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.2f, party, f.targets);
    if (mode != MultiplayerMode::Normal) {
        REQUIRE(f.arsenal.missiles().count() == 1);
        CHECK(f.arsenal.missiles().missile(0).velocity.z < 0);
        CHECK(hits == 1);
    }
}

TEST_CASE("hurt mode adds forward player melee and aiming only as a world-target fallback",
          "[game][screens][player-attacks][multiplayer-combat]") {
    Fixture f;
    std::array<PlayerRuntime, 2> party;
    party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 3}, 0);
    f.targets.players = party;
    for (const auto mode :
         {MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt}) {
        f.targets.multiplayer = mode;
        CHECK(f.attacks.aim(party[0].actor, Vec3{0, 0, 1}, f.targets).has_value() ==
              (mode == MultiplayerMode::Hurt));
        CHECK((f.attacks.meleeSense(party[0].actor, true, f.targets).range != MeleeRange::Beyond) ==
              (mode == MultiplayerMode::Hurt));
    }
    party[1].actor.place(Vec3{0, 0, -3});
    CHECK(f.attacks.meleeSense(party[0].actor, true, f.targets).range == MeleeRange::Beyond);
    party[1].actor.place(Vec3{0, 0, 3});
    party[1].departed = true;
    CHECK_FALSE(f.attacks.aim(party[0].actor, Vec3{0, 0, 1}, f.targets));
    party[1].departed = false;
    const auto root = turboAssets();
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 20};
    REQUIRE(enemies.spawn(spawn, {}));
    const auto aim = f.attacks.aim(party[0].actor, Vec3{0, 0, 1}, f.targets);
    REQUIRE(aim);
    CHECK(aim->z > 10);
}

TEST_CASE("class turbo areas harm other players only in hurt mode with shared effect cooldown",
          "[game][screens][player-attacks][multiplayer-combat]") {
    const auto root = turboAssets();
    for (const auto mode :
         {MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt}) {
        Fixture f;
        f.mode = mode;
        REQUIRE(f.classes.load(root / "pdata"));
        std::array<PlayerRuntime, 2> party;
        party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 3}, 0);
        party[0].figure = PlayerFigure::load(f.device, root, party[0].actor.save(), false);
        REQUIRE(party[0].figure);
        party[0].turbo.add(100);
        s32 hits = 0;
        f.targets.multiplayer = mode;
        f.targets.players = party;
        f.targets.hurt = [&](usize index, f32 damage, HurtKind, const PlayerImpact&) {
            CHECK(index == 1);
            CHECK(damage > 0);
            ++hits;
        };
        for (s32 frame = 0; frame < 60; ++frame) {
            party[0].figure->animate(0, 2, 1.0f / 30,
                                     frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
            f.attacks.updateTurbo(0, 2, 1.0f / 30, party, [](s32, usize) {});
            if (frame == 3) {
                f.mode = MultiplayerMode::Normal; // A launched effect keeps its original mask.
            }
            f.attacks.updateStrikes(1.0f / 30, party, f.targets);
            party[1].effectGap = std::max(0.0f, party[1].effectGap - 1.0f / 30);
        }
        CHECK(hits == (mode == MultiplayerMode::Hurt ? 1 : 0));
        f.attacks.clear();
    }
}

TEST_CASE("turbo point hits show their impact and reflective armor turns their visual flight",
          "[game][screens][player-attacks][multiplayer-combat]") {
    const auto root = turboAssets();
    const bool passThrough = GENERATE(false, true);
    writeTextFile(root / "pdata/WAR.json", std::string{R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboC1":0},"moveEffects":[{"tree":"BURST"}],"moveStrikes":[
      {"type":2,"startFrame":1,"hitRadius":1,"arc":-1,"offset":[0,2,2],
       "speedMin":30,"speedMax":30,"maxTime":6,"amount":70,"flags":64,
       "effect":0,"hitEffect":0,"damageType":)"} +
                                               (passThrough ? "3145728" : "0") + "}]}");
    for (const bool reflective : {false, true}) {
        Fixture f;
        f.mode = MultiplayerMode::Hurt;
        REQUIRE(f.classes.load(root / "pdata"));
        std::array<PlayerRuntime, 2> party;
        party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 10}, 0);
        party[0].figure = PlayerFigure::load(f.device, root, party[0].actor.save(), false);
        REQUIRE(party[0].figure);
        party[0].turbo.add(100);
        if (reflective) {
            party[1].effectGap = 1;
            party[1].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                                  powerup::kReflectShield, 0, 1);
        }
        s32 hits = 0;
        bool reflected = false;
        f.targets.hurt = [&](usize index, f32 damage, HurtKind, const PlayerImpact&) {
            CHECK(index == 1);
            CHECK(damage == 70);
            ++hits;
        };
        for (s32 frame = 0; frame < 30 && hits == 0 && !reflected; ++frame) {
            party[0].figure->animate(0, 2, 1.0f / 30,
                                     frame == 0 ? PlayerDeed::TurboFull : PlayerDeed::None);
            f.attacks.updateTurbo(0, 2, 1.0f / 30, party, [](s32, usize) {});
            f.attacks.updateStrikes(1.0f / 30, party, f.targets);
            reflected =
                f.attacks.strikes().count() != 0 && f.attacks.strikes().strike(0).facing.z < 0;
            f.effects.update(1.0f / 30);
        }
        if (reflective) {
            REQUIRE(reflected);
            CHECK(hits == 0);
            REQUIRE(f.effects.count() == 1);
            CHECK(f.effects.effect(0).velocity.z == -30);
            CHECK(f.attacks.strikes().strike(0).damage == 15);
            CHECK(f.attacks.strikes().strike(0).owner == 3);
        } else {
            CHECK(hits == 1);
            if (passThrough) {
                REQUIRE(f.attacks.strikes().count() == 1);
                CHECK(party[1].effectGap == 1);
                // The reflected Super was spent despite the next contact's shared cooldown.
                f.attacks.updateStrikes(1.0f / 30, party, f.targets);
                CHECK(hits == 1);
            }
            CHECK(f.attacks.strikes().count() == 0);
            REQUIRE(f.effects.count() == (passThrough ? 2 : 1));
            CHECK(f.effects.effect(0).name == "BURST");
            CHECK(f.effects.effect(0).velocity == Vec3{0});
            CHECK(party[1].effectGap == Approx(passThrough ? 1 : 2));
        }
        f.attacks.clear();
    }
}

TEST_CASE("player turbo areas respect distant item cover but cross it within ten units",
          "[game][screens][player-attacks][multiplayer-combat][assets]") {
    const auto assets =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto root = turboAssets();
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0},"moveEffects":[{"tree":"BURST"}],"moveStrikes":[
      {"type":4,"startFrame":1,"radius":35,"arc":-1,"offset":[0,2,0],
       "amount":4,"effect":0,"damageType":368}]})");
    LevelCatalog catalog;
    REQUIRE(catalog.load(assets));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    for (const s32 scenario : {0, 1, 2}) {
        CAPTURE(scenario);
        const bool near = scenario == 2;
        const bool removed = scenario == 1;
        Fixture f;
        f.mode = MultiplayerMode::Hurt;
        REQUIRE(f.classes.load(root / "pdata"));
        REQUIRE(f.world.load(f.device, assets, *level));
        f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
        REQUIRE(f.world.walls().size() == 5);
        REQUIRE(f.world.walls().standing(0));
        if (removed) {
            f.fixtures.strikeWall(0, 1000);
            REQUIRE_FALSE(f.world.walls().standing(0));
        }
        std::array<PlayerRuntime, 2> party;
        party[0].actor.spawn(3, {}, nullptr, Vec3{55, 0, near ? -5 : 0}, 0);
        party[1].actor.spawn(1, {}, nullptr, Vec3{55, 0, near ? -12 : -20}, 0);
        party[0].figure = PlayerFigure::load(f.device, root, party[0].actor.save(), false);
        REQUIRE(party[0].figure);
        party[0].turbo.add(100);
        s32 hits = 0;
        f.targets.hurt = [&](usize index, f32 damage, HurtKind, const PlayerImpact& impact) {
            CHECK(index == 1);
            CHECK(damage > 0);
            CHECK(damage < 5);
            CHECK((impact.flags & 0x170) == 0);
            CHECK((impact.flags & 0x1000000) != 0); // weak areas suppress the hit effect
            ++hits;
        };
        for (s32 frame = 0; frame < 60; ++frame) {
            party[0].figure->animate(0, 2, 1.0f / 30,
                                     frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
            f.attacks.updateTurbo(0, 2, 1.0f / 30, party, [](s32, usize) {});
            f.attacks.updateStrikes(1.0f / 30, party, f.targets);
            party[1].effectGap = std::max(0.0f, party[1].effectGap - 1.0f / 30);
        }
        CHECK(hits == (removed || near ? 1 : 0));
        if (!removed) {
            CHECK(f.world.walls().standing(0));
        }
        f.attacks.clear();
    }
}

/** The warrior's costume colour's effects with the fire weapon's glow and throw trees, and a
 * wizard costume beside the warrior's. */
void addElementalEffects(const std::filesystem::path& root) {
    for (const auto* name :
         {"PLAYERS/WAR/YEL", "PLAYERS/WAR/SFXYEL", "PLAYERS/WIZ/YEL", "PLAYERS/WIZ/SFXYEL"}) {
        const auto dir = root / name;
        std::filesystem::create_directories(dir);
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        // The costumes have a weapon hand and the weapon it holds.
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1},
          {"index":1,"name":"R_WRIST","file":"body.obj","meshTriangles":1},
          {"index":2,"name":"WEAP_YEL_HD1","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(dir);
    }
    for (const auto* cls : {"WAR", "WIZ"}) {
        writeTextFile(root / "PLAYERS" / cls / "YEL/animations.json",
                      std::string{R"({"trees":[{"name":")"} + cls + R"(_YEL",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
                   {"name":"HAND","object":"R_WRIST","parent":0,"position":[1,2,3]}],
          "sequences":[]}]})");
    }
    for (const auto* name : {"PLAYERS/WAR/SFXYEL", "PLAYERS/WIZ/SFXYEL"}) {
        writeTextFile(root / name / "animations.json", R"({"trees":[
          {"name":"WEAP_HOLD_RED",
           "nodes":[{"name":"XN","object":"BODY","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"ACTIVE","frames":0,"rate":30}]},
          {"name":"WEAP_TW_R",
           "nodes":[{"name":"XN","object":"BODY","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"ACTIVE","frames":0,"rate":30}]}]})");
    }
    writeTextFile(root / "pdata/WIZ.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600]})");
}

TEST_CASE("an elemental weapon glows in the hand while it is worn",
          "[game][screens][player-attacks][weapon-glow][damage-types]") {
    const auto root = turboAssets();
    addElementalEffects(root);
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    auto& inventory = player.actor.save().progress().inventory;
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    CHECK(f.effects.count() == 0);
    CHECK(player.weaponGlow.effect() == 0);
    inventory.addPowerup(powerup::kWeapon, 1, 0, 1); // a fire amulet
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    REQUIRE(f.effects.count() == 1);
    CHECK(player.weaponGlow.element() == 1);
    CHECK(player.weaponGlow.effect() != 0);
    const u32 lit = player.weaponGlow.effect();
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    CHECK(f.effects.count() == 1);
    CHECK(player.weaponGlow.effect() == lit);
    // The super shot fills the hand instead; taking the amulet off puts the glow out.
    inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 3, 1);
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    CHECK(f.effects.count() == 0);
    inventory.powerups[1].on = false;
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    CHECK(f.effects.count() == 1);
    inventory.powerups[0].on = false;
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    CHECK(f.effects.count() == 0);
    CHECK(player.weaponGlow.effect() == 0);
    f.attacks.clear();
}

TEST_CASE("an elemental weapon's throw carries its effect, the wizard's the effect alone",
          "[game][screens][player-attacks][weapon-glow][damage-types]") {
    const auto root = turboAssets();
    addElementalEffects(root);
    for (const s32 character : {0, 2}) {
        CAPTURE(character);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        auto& player = f.players[0];
        player.actor.save().character = character;
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        f.arsenal.launchWeapon(player.actor, player.figure.get(), Vec3{0, 0, 1}, 1, false);
        REQUIRE(f.arsenal.missiles().count() == 1);
        CHECK(f.arsenal.missiles().missile(0).rider == 0);
        player.actor.save().progress().inventory.addPowerup(powerup::kWeapon, 1, 0, 1);
        f.arsenal.launchWeapon(player.actor, player.figure.get(), Vec3{0, 0, 1}, 1, false);
        REQUIRE(f.arsenal.missiles().count() == 2);
        const auto& thrown = f.arsenal.missiles().missile(1);
        CHECK(thrown.rider != 0);
        CHECK((thrown.model == nullptr) == (character == 2));
        f.attacks.clear();
    }
}

TEST_CASE("gas damage queues pain without overlapping direct choking voices",
          "[game][player-attacks][gas-feedback]") {
    const auto root = turboAssets();
    const auto bank = root / "audio/WAR";
    std::filesystem::create_directories(bank);
    const std::vector<s16> tone(96000, 8192);
    const std::vector<s16> groan(24000, -4096);
    const std::array<test::NativeSoundSample, 2> bankSamples{
        {{48000, {tone.begin(), tone.end()}}, {48000, {groan.begin(), groan.end()}}}};
    test::writeNativeSoundBank(bank, R"({"sounds":[
      {"index":0,"name":"S_WARDIE1","id":0,"volume":127,"sequence":[{"sample":1}]},
      {"index":1,"name":"S_WARPOISON","id":1,"volume":127,"sequence":[{"sample":0}]},
      {"index":2,"name":"S_WARPAIN1","id":2,"volume":127,"sequence":[{"sample":0}]},
      {"index":3,"name":"S_WARPAIN2","id":3,"volume":127,"sequence":[{"sample":0}]},
      {"index":4,"name":"S_WARPAIN3","id":4,"volume":127,"sequence":[{"sample":0}]},
      {"index":5,"name":"S_WARPAIN4","id":5,"volume":127,"sequence":[{"sample":0}]}]})",
                               bankSamples);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    f.audio.open(root, &sounds, nullptr);
    f.attacks.bind({f.device, f.classes, f.world, f.weapons, f.effects, f.audio, &sounds, f.arsenal,
                    f.dimmer, &f.shake});
    auto& player = f.players[0];
    player.actor.save().progress().health = 1000;
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    PlayerHealth health;
    PlayerHealth::Events events;
    events.cry = [&](std::string_view cue) { f.attacks.cry(0, cue, f.players); };
    for (s32 pulse = 0; pulse < 6; ++pulse) {
        health.hurt(player, 10, HurtKind::Gas, true, false, 1, events);
    }
    CHECK(player.actor.save().health() == 940);
    CHECK(sounds.voiceCount() == 1); // The two-second bark occupies the bounded queue.
    CHECK(f.audio.barkBacklog() == Approx(2));
    health.hurt(player, 10, HurtKind::Pierce, true, false, 1, events);
    CHECK(sounds.voiceCount() == 2); // A spike groan is direct, not dropped by the bark queue.
    CHECK(f.audio.barkBacklog() == Approx(2));
    f.attacks.clear();
    f.audio.close();
}

TEST_CASE("weapon throw audio follows the worn amulet or special shot",
          "[game][player-attacks][play-audio]") {
    const auto root = turboAssets();
    const auto bank = root / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::vector<s16> tone(48000, 8192);
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {tone.begin(), tone.end()}}}};
    const std::array names{"S_AMULETFIRE", "S_AMULETLIGHTNI", "S_AMULETLIGHT", "S_AMULETACID",
                           "S_SUPERSHOT"};
    for (usize i = 0; i < names.size(); ++i) {
        CAPTURE(i);
        test::writeNativeSoundBank(bank,
                                   std::string{R"({"sounds":[{"index":0,"name":")"} + names[i] +
                                       R"(","id":0,"volume":127,"sequence":[{"sample":0}]}]})",
                                   bankSamples);
        AudioMixer mixer(48000);
        SoundPlayer sounds(mixer);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        f.audio.open(root, &sounds, nullptr);
        auto& player = f.players[0];
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        const u32 flag = i == 4 ? powerup::kSuperShot : static_cast<u32>(i + 1);
        player.actor.save().progress().inventory.addPowerup(powerup::kWeapon, flag, 3, 1);
        if (i == 4) {
            f.arsenal.launchSuperShot(player.actor, player.figure.get());
        } else {
            f.arsenal.launchWeapon(player.actor, player.figure.get(), Vec3{0, 0, 1}, 1, false);
        }
        CHECK(sounds.voiceCount() == 1);
        std::array<f32, 512> samples{};
        mixer.mix(samples);
        // Command 127 becomes 126: -3.1 dB DCS master and -3 dB center pan.
        CHECK(samples.back() == Approx(0.25f * std::pow(10.0f, -61.0f / 200.0f)));
        f.attacks.clear();
        f.audio.close();
    }
}

TEST_CASE("turbo contacts damage nearby enemies in every direction and reject distant floors",
          "[game][screens][player-attacks][turbo-contacts]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    EnemyScales scales;
    scales.health = 100;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    std::array<s32, 4> ids{};
    const std::array positions{Vec3{0, 0, 5}, Vec3{0, 0, -5}, Vec3{0, 40, 5}, Vec3{0, 0, 50}};
    for (usize i = 0; i < positions.size(); ++i) {
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.placed = true;
        spawn.position = positions[i];
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        ids[i] = *id;
    }
    const f32 before = enemies.healthOf(ids[0]);
    player.turbo.add(100);
    for (s32 frame = 0; frame < 30; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
        f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
    }
    CHECK(enemies.healthOf(ids[0]) < before);
    CHECK(enemies.healthOf(ids[1]) == enemies.healthOf(ids[0]));
    CHECK(enemies.healthOf(ids[2]) == before);
    CHECK(enemies.healthOf(ids[3]) == before);
    CHECK(player.turbo.held() == 60);
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("authored player effects shake the camera without requiring an effect mesh",
          "[game][player-attacks][camera-shake]") {
    const auto root = turboAssets();
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0},"moveStrikes":[
      {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50,"effect":0}],
      "moveEffects":[{"flags":2,"tree":"NONE"}]})");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    player.turbo.add(100);
    for (s32 frame = 0; frame < 4; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
    }
    CHECK(f.shake.active());
    CHECK(glm::length(f.shake.offset()) == Approx(0.1f));
    CHECK(f.effects.count() == 0);
}

TEST_CASE("player particle records start on their strike frame and follow their attachment",
          "[game][player-attacks][player-particles]") {
    const auto root = turboAssets();
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0},"moveStrikes":[
      {"type":0,"startFrame":3,"effect":0}],"moveEffects":[
      {"flags":33554432,"tree":"SKIN","sound":"BODY","offset":[1,2,3],
       "lifetime":0.5,"radius":3,"alphaMod":150,"scale":2,"next":1},
      {"flags":16777216,"tree":"SKIN","sound":"MISSING_NODE","offset":[0,4,0],
       "lifetime":0.1,"radius":2,"alphaMod":300}]})");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    player.turbo.add(100);
    for (s32 frame = 0; frame < 2; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
        CHECK(f.effects.count() == 0);
    }
    for (s32 frame = 0; frame < 4 && f.effects.count() == 0; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30, PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
    }
    REQUIRE(f.effects.count() == 2);
    const auto first = f.effects.effect(0).id;
    const auto& descriptor = f.effects.effect(0).trails.emitter(0).descriptor();
    CHECK(descriptor.emitFrames == 15);
    CHECK(descriptor.fadeFrames == 1);
    CHECK(descriptor.particleLife == 6);
    CHECK(descriptor.particleFade == 0);
    CHECK(descriptor.rate[0] == 3);
    CHECK(descriptor.speed == Approx(0.05f));
    CHECK(descriptor.width.lifeStart == 1);
    CHECK(f.effects.effect(1).trails.emitter(0).descriptor().particleLife == 15);
    CHECK(f.effects.effect(1).trails.emitter(0).descriptor().particleFade == 15);
    player.actor.spawn(3, player.actor.save(), nullptr, {12, 3, 7}, 0.8f);
    f.attacks.updateStrikes(0, f.players, f.targets);
    const Mat4 body =
        PlayerFigure::bodyPlacement(player.actor.transform(), player.actor.save(),
                                    PowerupEffects::of(player.actor.save().progress().inventory));
    const Vec3 expected = Vec3{player.figure->attachment(body, "BODY").value() * Vec4{1, 2, 3, 1}};
    CHECK(glm::distance(f.effects.effect(0).position, expected) < 0.001f);
    CHECK(glm::distance(f.effects.effect(1).position, Vec3{body * Vec4{0, 4, 0, 1}}) < 0.001f);
    f.effects.update(1.0f / 30);
    CHECK(f.effects.effect(0).trails.particleCount() == 3);
    f.device.draws.clear();
    f.effects.draw(f.device, Mat4{1}, {});
    CHECK_FALSE(f.device.draws.empty());
    for (s32 frame = 0; frame < 22; ++frame) {
        f.effects.update(1.0f / 30);
    }
    CHECK_FALSE(f.effects.playing(first));
    REQUIRE(f.effects.count() == 1); // the other kind's half-second fade survives
    f.attacks.clear();
    CHECK(f.effects.count() == 0);
}

TEST_CASE("native wizard particle chains emit visible particles from both hands",
          "[game][player-attacks][player-particles][assets]") {
    const auto root = test::assetOrSkip("PDATA/WIZ.WAD").parent_path().parent_path();
    for (const s32 character : {2, 10}) {
        CAPTURE(character);
        Fixture f;
        REQUIRE(f.classes.load(root / "PDATA"));
        REQUIRE(f.weapons.load(root / "WEAPONS"));
        auto& player = f.players[0];
        player.actor.save().character = character;
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        player.turbo.add(100);
        bool particles = false;
        bool pairedHands = false;
        for (s32 frame = 0; frame < 90; ++frame) {
            player.figure->animate(0, 2, 1.0f / 30,
                                   frame == 0 ? PlayerDeed::TurboFull : PlayerDeed::None);
            f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
            f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
            f.effects.update(1.0f / 30);
            std::optional<Vec3> hand;
            for (usize i = 0; i < f.effects.count(); ++i) {
                const auto& effect = f.effects.effect(i);
                if (effect.tree == nullptr) {
                    CHECK(effect.name == "WIZ_HEAD_Y");
                    CHECK(effect.trails.textureOf(0) != nullptr);
                    particles |= effect.trails.particleCount() > 0;
                    if (effect.trails.emitter(0).descriptor().particleLife == 6) {
                        if (hand) {
                            pairedHands |= glm::distance(*hand, effect.position) > 0.1f;
                        }
                        hand = effect.position;
                    }
                }
            }
        }
        CHECK(particles);
        CHECK(pairedHands);
        f.attacks.clear();
        for (usize i = 0; i < f.effects.count(); ++i) {
            CHECK(f.effects.effect(i).tree != nullptr);
        }
    }
}

TEST_CASE("a charge throws down the enemy it runs into, once a charge",
          "[game][screens][player-attacks][charge]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    EnemyScales scales;
    scales.health = 100;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto spawnAt = [&](const Vec3& position) {
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.placed = true;
        spawn.position = position;
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        return *id;
    };
    const s32 against = spawnAt(Vec3{0, 0, 1.5f});
    const s32 afar = spawnAt(Vec3{0, 0, 20});
    const f32 whole = enemies.healthOf(against);
    f.attacks.ramBarrels(0, f.players, f.targets);
    CHECK(enemies.healthOf(against) < whole - 20.0f); // thirty-two, less its armour
    CHECK(enemies.healthOf(afar) == whole);
    enemies.update(2, 1.0f / 30, {});
    CHECK(enemies.pushCountOf(against) == 1); // thrown down, back the way the charge went
    const f32 once = enemies.healthOf(against);
    f.attacks.ramBarrels(0, f.players, f.targets);
    CHECK(enemies.healthOf(against) == once); // not again in the same charge
    f.opponents.close();
}

TEST_CASE("flying turbo strikes reach short enemies without dealing damage every frame",
          "[game][screens][player-attacks][turbo-contacts]") {
    const auto root = turboAssets();
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
        auto& player = f.players[0];
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        EnemyScales scales;
        scales.health = 100;
        auto& enemies = f.opponents.enemies();
        enemies.open(f.device, root, nullptr, 4, scales, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.placed = true;
        spawn.position = Vec3{0, 0, 20};
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        const f32 before = enemies.healthOf(*id);
        player.turbo.add(100);
        const f32 seconds = 1.0f / static_cast<f32>(hz);
        s32 contacts = 0;
        for (s32 frame = 0; frame < hz * 2; ++frame) {
            player.figure->animate(0, 1, seconds,
                                   frame == 0 ? PlayerDeed::TurboFull : PlayerDeed::None);
            f.attacks.updateTurbo(0, 1, seconds, f.players, [](s32, usize) {});
            const f32 prior = enemies.healthOf(*id);
            f.attacks.updateStrikes(seconds, f.players, f.targets);
            contacts += enemies.healthOf(*id) < prior ? 1 : 0;
        }
        CHECK(enemies.healthOf(*id) < before);
        CHECK(contacts == 1);
        f.attacks.clear();
        f.opponents.close();
    }
}

TEST_CASE("every native class can damage enemies with both turbo attacks",
          "[game][screens][player-attacks][turbo-roster][assets]") {
    const auto root = test::assetOrSkip("PDATA/JES.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    for (s32 character = 0; character < kSumnerClass; ++character) {
        CAPTURE(classCode(character));
        for (const auto deed : {PlayerDeed::TurboStrong, PlayerDeed::TurboFull}) {
            CAPTURE(static_cast<s32>(deed));
            Fixture f;
            REQUIRE(f.classes.load(root / "pdata"));
            f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1},
                             f.players);
            auto& player = f.players[0];
            player.actor.save().character = character;
            player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
            REQUIRE(player.figure);
            REQUIRE(player.figure->animator().canBegin(deed));
            auto& enemies = f.opponents.enemies();
            EnemyScales scales;
            scales.health = 100;
            enemies.open(f.device, root, nullptr, 4, scales, 1);
            REQUIRE(enemies.loadKind(kGruntKind));
            EnemySpawn spawn;
            spawn.kind = kGruntKind;
            spawn.placed = true;
            spawn.position = Vec3{0, 0, -5};
            const auto near = enemies.spawn(spawn, {});
            REQUIRE(near);
            spawn.position.z = 35;
            const auto far = enemies.spawn(spawn, {});
            REQUIRE(far);
            const f32 before = enemies.healthOf(*near);
            player.turbo.add(100);
            for (s32 frame = 0; frame < 150; ++frame) {
                player.figure->animate(0, 2, 1.0f / 30, frame == 0 ? deed : PlayerDeed::None);
                f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
                f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
                f.effects.update(1.0f / 30);
            }
            CHECK(enemies.healthOf(*near) < before);
            if (deed == PlayerDeed::TurboFull) {
                CHECK(enemies.healthOf(*far) < before);
            }
            f.attacks.clear();
            f.opponents.close();
        }
    }
}

TEST_CASE("strike carriers use primary SFXX duration without replacing flying maxTime",
          "[game][screens][player-attacks][alpha-effect-lifetime]") {
    const auto [kind, duration, expected] = GENERATE(
        std::tuple{MoveStrike::kBursts, 4.0f, 4.0f}, std::tuple{MoveStrike::kBursts, 0.0f, 2.0f},
        std::tuple{MoveStrike::kFlies, 4.0f, 6.0f}, std::tuple{MoveStrike::kFlies, 0.0f, 6.0f});
    const auto root = turboAssets();
    writeTextFile(root / "pdata/WAR.json",
                  std::string{R"({"height":6,"width":2,"fight":[200,600],"speed":[200,600],
      "armor":[200,600],"magic":[200,600],"moves":{"turboB":0},
      "moveEffects":[{"tree":"BURST","lifetime":)"} +
                      std::to_string(duration) + R"(}],"moveStrikes":[{"type":)" +
                      std::to_string(kind) + R"(,"startFrame":1,"radius":12,"hitRadius":2,
      "arc":-1,"amount":10,"effect":0,"maxTime":6}]})");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    player.turbo.add(50);
    for (s32 frame = 0; frame < 4; ++frame) {
        player.figure->animate(0, 1, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 1, 1.0f / 30, f.players, [](s32, usize) {});
    }
    REQUIRE(f.attacks.strikes().count() == 1);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.attacks.strikes().strike(0).secondsLeft == Approx(expected));
    REQUIRE(f.effects.remaining(f.effects.effect(0).id));
    CHECK(*f.effects.remaining(f.effects.effect(0).id) == Approx(expected));
}

TEST_CASE("Warrior and Minotaur combo damage lasts for the four-second native SFXX",
          "[game][screens][player-attacks][alpha-effect-lifetime][assets]") {
    const s32 character = GENERATE(0, 8);
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    test::assetOrSkip("PDATA/MIN.WAD");
    test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.actor.save().character = character;
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    const auto* stats = f.classes.stats(character);
    REQUIRE(stats);
    REQUIRE(stats->moveStrikes.size() > 9);
    const auto& row = stats->moveStrikes[9];
    CHECK(row.type == MoveStrike::kBursts);
    CHECK(row.amount == 10);
    REQUIRE(row.effect >= 0);
    const auto& setting = stats->moveEffects.at(static_cast<usize>(row.effect));
    CHECK(setting.tree == "WAR_COMBO2");
    CHECK(setting.lifetime == 4);
    auto* archive = player.figure->effects();
    REQUIRE(archive);
    const auto tree = archive->trees.find(setting.tree);
    REQUIRE(tree);
    const auto& sequences = archive->trees.tree(*tree).sequences;
    REQUIRE_FALSE(sequences.empty());
    CHECK(sequences.front().frames == 0);

    u32 shown = 0;
    player.turbo.add(100);
    for (s32 frame = 0; frame < 90 && shown == 0; ++frame) {
        player.figure->animate(0, 1, 1.0f / 30, frame == 0 ? PlayerDeed::Combo : PlayerDeed::None);
        f.attacks.updateTurbo(0, 1, 1.0f / 30, f.players, [](s32, usize) {});
        for (usize i = 0; i < f.effects.count(); ++i) {
            if (f.effects.effect(i).name == setting.tree) {
                shown = f.effects.effect(i).id;
            }
        }
        if (shown == 0) {
            f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
            f.effects.update(1.0f / 30);
        }
    }
    REQUIRE(shown != 0);
    REQUIRE(f.attacks.strikes().count() > 0);
    const auto& carrier = f.attacks.strikes().strike(f.attacks.strikes().count() - 1);
    const u32 strike = carrier.id;
    CHECK(carrier.damage == row.amount);
    CHECK(carrier.secondsLeft == Approx(4));
    REQUIRE(f.effects.remaining(shown));
    CHECK(*f.effects.remaining(shown) == Approx(4));
    for (const f32 seconds : {2.0f, 1.9f}) {
        f.attacks.updateStrikes(seconds, f.players, f.targets);
        f.effects.update(seconds);
        REQUIRE(f.attacks.strikes().find(strike));
        REQUIRE(f.effects.remaining(shown));
        CHECK(f.attacks.strikes().find(strike)->secondsLeft ==
              Approx(*f.effects.remaining(shown)).margin(0.001f));
    }
    f.attacks.updateStrikes(0.2f, f.players, f.targets);
    f.effects.update(0.2f);
    CHECK_FALSE(f.attacks.strikes().find(strike));
    CHECK_FALSE(f.effects.playing(shown));
}

TEST_CASE("area effects follow the posed root unless their SFXX requests a detached snapshot",
          "[game][screens][player-attacks][alpha-attachments]") {
    for (const s32 flags : {0, 1, 0x40}) {
        CAPTURE(flags);
        const auto root = turboAssets();
        writeTextFile(root / "PLAYERS/WAR/ANIM/animations.json", R"({"trees":[{"name":"WAR",
          "nodes":[{"name":"BODY","parent":-1,"position":[0,3,0]}],"sequences":[
          {"name":"READY","frames":60,"rate":30},
          {"name":"ATTPWRB","frames":60,"rate":30}]}]})");
        writeTextFile(root / "pdata/WAR.json",
                      std::string{R"({"height":6,"width":2,"fight":[200,600],"speed":[200,600],
          "armor":[200,600],"magic":[200,600],"moves":{"turboB":0},
          "moveEffects":[{"tree":"BURST","offset":[1,0,2],"flags":)"} +
                          std::to_string(flags) + R"(}],"moveStrikes":[
          {"type":4,"startFrame":1,"radius":12,"arc":0.7,"delay":0.1,
           "amount":50,"effect":0}]})");
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        auto& player = f.players[0];
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        player.turbo.add(100);
        for (s32 frame = 0; frame < 4; ++frame) {
            player.figure->animate(0, 1, 1.0f / 30,
                                   frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
            f.attacks.updateTurbo(0, 1, 1.0f / 30, f.players, [](s32, usize) {});
        }
        REQUIRE(f.effects.count() == 1);
        REQUIRE(f.attacks.strikes().count() == 1);
        const Vec3 start{1, flags == 0 ? 3.0f : 0.0f, 2};
        CHECK(glm::distance(f.effects.effect(0).position, start) < 0.001f);
        player.actor.place({10, 4, 5});
        player.actor.turnTo(std::numbers::pi_v<f32> / 2);
        f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
        const Vec3 expected = flags == 0x40 ? start : Vec3{12, flags == 0 ? 7.0f : 4.0f, 4};
        CHECK(glm::distance(f.effects.effect(0).position, expected) < 0.001f);
        CHECK(glm::distance(f.attacks.strikes().strike(0).position, expected) < 0.001f);
        const Vec3 forward = flags == 0x40 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        CHECK(glm::distance(Vec3{f.effects.effect(0).transform()[2]}, forward) < 0.001f);
        CHECK(glm::distance(f.attacks.strikes().strike(0).facing, forward) < 0.001f);
        f.attacks.clear();
        CHECK(f.effects.count() == (flags == 0x40 ? 1 : 0));
    }
}

TEST_CASE("Sonic Boom's chained ring turns with its caster and retains its authored height",
          "[game][screens][player-attacks][alpha-attachments][assets]") {
    const auto root = test::assetOrSkip("PDATA/JES.WAD").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/SFXYEL/ANIM.PS2");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& player = f.players[0];
    player.actor.save().character = 7;
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    player.turbo.add(50);
    for (s32 frame = 0; frame < 4; ++frame) {
        player.figure->animate(0, 1, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 1, 1.0f / 30, f.players, [](s32, usize) {});
    }
    REQUIRE(f.effects.count() == 2);
    REQUIRE(f.effects.effect(0).name == "JES_PWRB1");
    REQUIRE(f.effects.effect(1).name == "JES_PWRB2");
    player.actor.place({10, 4, 5});
    player.actor.turnTo(std::numbers::pi_v<f32> / 2);
    f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
    for (usize i = 0; i < f.effects.count(); ++i) {
        CHECK(glm::distance(f.effects.effect(i).position, player.actor.position()) < 0.001f);
        CHECK(glm::distance(Vec3{f.effects.effect(i).transform()[2]}, Vec3{1, 0, 0}) < 0.001f);
    }
    const auto& ring = f.effects.effect(1);
    REQUIRE(ring.pose.matrices().size() >= 2);
    CHECK((ring.transform() * ring.pose.matrices()[1])[3].y == Approx(4 + 1.92348f));
}

TEST_CASE("Sorceress power attacks draw the SFXX row's colour rather than costume white",
          "[game][screens][player-attacks][alpha-kiss-color][assets]") {
    const auto root = test::assetOrSkip("PDATA/SOR.WAD").parent_path().parent_path();
    const s32 row = GENERATE(0, 1, 2);
    const s32 costume = GENERATE(0, 2);
    CAPTURE(row, costume);
    Fixture f;
    REQUIRE(f.classes.load(root / "PDATA"));
    auto& player = f.players[0];
    player.actor.save().character = 6;
    player.actor.save().color = costume;
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    const auto* stats = f.classes.stats(6);
    REQUIRE(stats);
    CHECK(stats->moves.turboAClose == 0);
    CHECK(stats->moves.turboAStep == 1);
    CHECK(stats->moves.turboAThrow == 2);
    const auto step = [&](PlayerDeed deed) {
        player.figure->animate(0, 2, 1.0f / 30, deed);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
    };
    if (row == 2) {
        step(PlayerDeed::StrongAttack);
    } else {
        step(PlayerDeed::Melee);
        if (row == 1) {
            const auto first = player.figure->animator().action();
            step(PlayerDeed::None);
            step(PlayerDeed::Melee);
            for (s32 frame = 0; frame < 120 && player.figure->animator().action() == first;
                 ++frame) {
                step(PlayerDeed::Melee);
            }
            REQUIRE(player.figure->animator().meleeChain() == 2);
        }
        step(PlayerDeed::None);
        step(PlayerDeed::MeleeSlow);
    }
    for (s32 frame = 0; frame < 180 && f.effects.count() == 0; ++frame) {
        step(PlayerDeed::None);
    }
    REQUIRE(f.effects.count() == 1);
    REQUIRE(f.effects.effect(0).name == "SOR_PWRA1");
    // Native SOR.WAD SFXX rows 0/1/2, +0x4C: 00FFFF00 / 00FF0000 / 0000FF00.
    // DoPlyrSfx -> MBTreeSetColor sets RGB; its high byte is not tree opacity.
    constexpr std::array<Color, 3> kColors{Color::rgba(255, 255, 0), Color::rgba(255, 0, 0),
                                           Color::rgba(0, 255, 0)};
    const Color expected = kColors[static_cast<usize>(row)];
    CHECK(f.effects.effect(0).tint == expected);
    f.effects.update(0.2f);
    f.device.draws.clear();
    WorldLighting lighting;
    lighting.ambient = Vec3{1};
    lighting.lightColor = Vec3{0};
    f.effects.draw(f.device, Mat4{1}, lighting);
    REQUIRE_FALSE(f.device.draws.empty());
    usize visible = 0;
    for (const auto& draw : f.device.draws) {
        REQUIRE(draw.texture != nullptr);
        CHECK(draw.texture != &f.device.whiteTexture());
        for (const auto& vertex : draw.vertices) {
            if (vertex.color.a > 0) {
                ++visible;
                CHECK(vertex.color.r == expected.r);
                CHECK(vertex.color.g == expected.g);
                CHECK(vertex.color.b == expected.b);
            }
        }
    }
    CHECK(visible > 0);
}

TEST_CASE("Spell Storm launches three knights after birth and plays its three decoys once",
          "[game][screens][player-attacks][alpha-effects][assets]") {
    const auto root = test::assetOrSkip("PDATA/SOR.WAD").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/SOR/SFXYEL/ANIM.PS2");
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        auto& player = f.players[0];
        player.actor.save().character = 6;
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        const auto* stats = f.classes.stats(6);
        REQUIRE(stats);
        const auto rows = stats->strikesOf(stats->moves.turboC2);
        REQUIRE(rows.size() == 6);
        for (usize i = 0; i < rows.size(); ++i) {
            const auto& row = stats->moveStrikes[static_cast<usize>(rows[i])];
            CHECK(row.speed == (i < 3 ? 20 : 0));
            CHECK((row.amount > 0) == (i < 3));
            CHECK((row.loopEffect >= 0) == (i < 3));
            CHECK((row.flags & 0x800) != 0);
        }
        struct Knight {
            u32 id;
            Vec3 origin;
            bool launches;
        };
        std::vector<Knight> knights;
        bool paid = false;
        player.turbo.add(100);
        const f32 seconds = 1.0f / static_cast<f32>(hz);
        for (s32 step = 0; step < hz * 9; ++step) {
            player.figure->animate(0, 1, seconds,
                                   step == 0 ? PlayerDeed::TurboFull : PlayerDeed::None);
            f.attacks.updateTurbo(0, 1, seconds, f.players, [](s32, usize) {});
            if (!paid && f.attacks.strikes().count() > 0) {
                CHECK(player.turbo.held() == 0);
                paid = true;
            }
            for (usize i = 0; i < f.effects.count(); ++i) {
                const auto& effect = f.effects.effect(i);
                if (effect.name != "SOR_PWRC3A") {
                    continue;
                }
                const auto found = std::ranges::find(knights, effect.id, &Knight::id);
                if (found == knights.end()) {
                    knights.push_back({effect.id, effect.position, !effect.then.empty()});
                } else if (effect.lived < 1) {
                    CHECK(glm::distance(effect.position, found->origin) < 0.001f);
                }
                if (effect.then.empty()) {
                    CHECK_FALSE(effect.repeats);
                    CHECK_FALSE(effect.timed);
                    if (effect.lived > 1.08f && effect.lived < 1.16f) {
                        CHECK(glm::length(Vec3{effect.transform()[0]}) < 0.85f);
                    }
                }
            }
            f.attacks.updateStrikes(seconds, f.players, f.targets);
            f.effects.update(seconds);
            if (step == hz * 2) {
                // Already detached, the knights do not turn with the caster.
                player.actor.place({100, 0, 0});
                player.actor.turnTo(std::numbers::pi_v<f32> / 2);
            }
            if (step == hz * 3 || step == hz * 7) {
                REQUIRE(f.effects.count() == 3);
                for (usize i = 0; i < f.effects.count(); ++i) {
                    CHECK(f.effects.effect(i).name == "SOR_PWRC3B");
                    CHECK(f.effects.effect(i).position.z > 5);
                    CHECK(std::abs(f.effects.effect(i).position.x) < 5);
                }
            }
        }
        REQUIRE(knights.size() == 6);
        CHECK(std::ranges::count(knights, true, &Knight::launches) == 3);
        CHECK(f.effects.count() == 0);
        CHECK(paid);
    }
}

TEST_CASE("Sonic Boom reaches distant enemies during the clap rather than the wind-up",
          "[game][screens][player-attacks][sonic-timing][assets]") {
    const auto root = test::assetOrSkip("PDATA/JES.WAD").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/JES/SFXYEL/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    for (const s32 hz : {30, 60, 120}) {
        CAPTURE(hz);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
        auto& player = f.players[0];
        player.actor.save().character = 7;
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        const auto* stats = f.classes.stats(7);
        REQUIRE(stats);
        const auto& row = stats->moveStrikes[static_cast<usize>(stats->moves.turboB)];
        REQUIRE(row.startFrame == 1);
        REQUIRE(row.delay == Approx(0.5f));
        REQUIRE(row.radius == 12);
        auto& enemies = f.opponents.enemies();
        EnemyScales scales;
        scales.health = 100;
        enemies.open(f.device, root, nullptr, 4, scales, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.placed = true;
        spawn.position = Vec3{0, 0, 3};
        const auto near = enemies.spawn(spawn, {});
        REQUIRE(near);
        spawn.position.z = 11;
        const auto far = enemies.spawn(spawn, {});
        REQUIRE(far);
        std::array<f32, 2> firstFrame{-1, -1};
        std::array<s32, 2> contacts{};
        const std::array<s32, 2> ids{*near, *far};
        player.turbo.add(50);
        const f32 seconds = 1.0f / static_cast<f32>(hz);
        for (s32 step = 0; step < hz * 3; ++step) {
            player.figure->animate(0, 1, seconds,
                                   step == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
            f.attacks.updateTurbo(0, 1, seconds, f.players, [](s32, usize) {});
            const std::array<f32, 2> before{enemies.healthOf(*near), enemies.healthOf(*far)};
            f.attacks.updateStrikes(seconds, f.players, f.targets);
            f.effects.update(seconds);
            for (usize i = 0; i < ids.size(); ++i) {
                if (enemies.healthOf(ids[i]) < before[i]) {
                    if (contacts[i]++ == 0) {
                        firstFrame[i] = player.figure->animator().player().frame();
                    }
                }
            }
        }
        CAPTURE(firstFrame, contacts);
        CHECK(firstFrame[0] >= 15);
        CHECK(firstFrame[0] <= 18);
        CHECK(firstFrame[1] >= 30);
        CHECK(firstFrame[1] <= 43);
        CHECK(contacts[0] == 1);
        CHECK(contacts[1] == 1);
        f.attacks.clear();
        f.opponents.close();
    }
}

TEST_CASE("Jester turbo damage reaches bosses great creatures and generators",
          "[game][screens][player-attacks][turbo-roster][assets]") {
    const auto root = test::assetOrSkip("PDATA/JES.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GOLEM/LEVELG/ANIM.PS2");
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& player = f.players[0];
    player.actor.save().character = 7;
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    auto& bosses = f.opponents.bosses();
    auto& critters = f.opponents.critters();
    auto& enemies = f.opponents.enemies();
    auto& generators = f.opponents.generators();
    bosses.open(f.device, root, nullptr, {}, 'G');
    REQUIRE(bosses.spawn(41, {0, 0, -5}, 0));
    bosses.wake(); // asleep it takes nothing
    critters.open(f.device, root, nullptr, {}, 'G');
    const auto golem = critters.spawn(CombatantKind::Golem, {5, 0, 0}, 0);
    REQUIRE(golem);
    enemies.open(f.device, root, nullptr, 4, {}, 1);
    ItemInfo generator;
    generator.type = ItemInfo::kGenerator;
    generator.name = "BOSSGEN";
    generator.hitPoints = 500;
    generator.height = 6;
    generator.xSize = 2;
    generator.zSize = 2;
    Mat4 placement{1};
    placement[3] = Vec4{-5, 0, 0, 1};
    REQUIRE(generators.placeBoss(f.device, generator, f.weapons, enemies, kGruntKind, placement,
                                 nullptr));
    const f32 bossHealth = bosses.view().health;
    const f32 golemHealth = critters.healthOf(*golem);
    const f32 generatorHealth = generators.healthOf(0);
    player.turbo.add(100);
    for (s32 frame = 0; frame < 45; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
        f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
    }
    CHECK(bosses.view().health < bossHealth);
    CHECK(critters.healthOf(*golem) < golemHealth);
    CHECK(generators.healthOf(0) < generatorHealth);
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("retail item attacks play authored effects and spend one charge on the animation event",
          "[game][items][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    for (const u32 mask : {powerup::kFireBreath, powerup::kAcidBreath, powerup::kLightningBreath,
                           powerup::kSkorneHorns, powerup::kSkorneMask, powerup::kThunderHammer}) {
        Fixture f;
        CAPTURE(mask);
        LevelCatalog catalog;
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName("G1");
        REQUIRE(level);
        REQUIRE(f.world.load(f.device, root, *level));
        REQUIRE(f.weapons.load(root / "WEAPONS"));
        auto& player = f.players[0];
        player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
        REQUIRE(player.figure);
        const bool hammer = mask == powerup::kThunderHammer;
        const s32 kind = hammer ? powerup::kWeapon : powerup::kSpecial;
        auto& inventory = player.actor.save().progress().inventory;
        inventory.addPowerup(kind, mask, 2, -1);
        const auto item = ItemAttack::select(PowerupEffects::of(inventory));
        REQUIRE(item);
        const auto deed = f.attacks.attackDeed(player.actor, false, f.targets);
        REQUIRE(deed == item->deed);
        player.figure->animate(0, 2, 1.0f / 30, deed);
        for (s32 frame = 0;
             frame < 180 && player.figure->animator().itemReleased() == PlayerDeed::None; ++frame) {
            player.figure->animate(0, 2, 1.0f / 30);
        }
        REQUIRE(player.figure->animator().itemReleased() == deed);
        f.attacks.useItemAttack(0, f.players);
        CHECK(f.shake.active() == hammer);
        if (hammer) {
            CHECK(glm::length(f.shake.offset()) == Approx(0.3f));
        }
        REQUIRE(f.effects.count() == 1);
        CHECK(f.effects.effect(0).name == item->tree);
        CHECK(f.effects.effect(0).attachment.has_value());
        const auto* slot = inventory.powerup(kind, mask);
        REQUIRE(slot);
        CHECK(slot->charge == (item->chargeKind != 0 ? 1 : 2));
        for (s32 frame = 0; frame < 20; ++frame) {
            f.effects.update(1.0f / 30);
            f.effects.draw(f.device, Mat4{1}, f.world.fullLighting());
        }
        CHECK_FALSE(f.device.draws.empty());
        f.attacks.clear();
        CHECK(f.effects.count() == 0);
    }
}

TEST_CASE("potion magic damages survivors once and drives knockdown through get-up",
          "[game][screens][player-attacks][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    EnemyScales scales;
    scales.health = 10;
    f.opponents.enemies().open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(f.opponents.enemies().loadKind(13));
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 2};
    const auto enemy = f.opponents.enemies().spawn(spawn, {});
    REQUIRE(enemy);
    const f32 before = f.opponents.enemies().healthOf(*enemy);
    SECTION("used potion") {
        auto& inventory = f.players[0].actor.save().progress().inventory;
        inventory.addPotions(1, 1);
        f.attacks.usePotion(0, f.players);
        CHECK(inventory.potions.empty());
    }
    SECTION("thrown potion hits with splash rather than ordinary projectile damage") {
        MissileLaunch potion;
        potion.owner = 3;
        potion.position = Vec3{0, 1, 0};
        potion.velocity = Vec3{0, 0, 50};
        potion.spec = &MissileSpec::potion();
        potion.potion = 1;
        potion.potency = 16;
        potion.damage = 40;
        REQUIRE(f.arsenal.missiles().launch(potion));
    }
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    REQUIRE(f.opponents.enemies().healthOf(*enemy) < before);
    REQUIRE(f.opponents.enemies().healthOf(*enemy) > 0);
    const f32 after = f.opponents.enemies().healthOf(*enemy);
    f.opponents.enemies().update(2, 1.0f / 30, {});
    REQUIRE(f.opponents.enemies().animatorOf(*enemy)->action() == EnemyAction::HitReact2);
    bool gotUp = false;
    for (s32 frame = 0; frame < 240; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
        f.opponents.enemies().update(2, 1.0f / 30, {});
        gotUp = gotUp || f.opponents.enemies().animatorOf(*enemy)->action() == EnemyAction::GetUp;
    }
    CHECK(gotUp);
    CHECK(f.opponents.enemies().healthOf(*enemy) == after);
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("scene projectile updates present retail world impacts once and preserve potion bursts",
          "[game][screens][player-attacks][projectile-impact][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::assetOrSkip("audio/COMMON.vbk");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, f.world.audio());
    CHECK(f.world.wallHitSound() == "S_WEAPONHITWOOD");
    f.arsenal.bind({f.device, f.classes, f.weapons, f.world.collision(), f.effects, f.audio,
                    &sounds, f.world.wallHitSound()});
    const Vec3 start{24.375f, 10, 2.5f};
    const auto floor = f.world.collision().floorAt(start, 20, 50);
    REQUIRE(floor);
    MissileLaunch launch;
    launch.position = {start.x, floor->y + 3, start.z};
    launch.velocity = Vec3{0, -5, 0};
    launch.owner = 3;
    launch.spec = &MissileSpec::of(0);
    std::string_view expected = "SPARKS";
    SECTION("ordinary weapon") {}
    SECTION("jester bomb") {
        launch.spec = &MissileSpec::of(7);
        expected = "EXPSMALL";
    }
    SECTION("potion keeps its own effect and sound without sparks") {
        launch.spec = &MissileSpec::potion();
        launch.potion = 1;
        launch.potency = 8;
        expected = "MP_FIRE";
    }
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(1, f.players, f.targets);
    REQUIRE(f.arsenal.missiles().count() == 0);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == expected);
    CHECK(sounds.voiceCount() == 1);
    f.attacks.updateProjectiles(1, f.players, f.targets);
    CHECK(f.effects.count() == 1);
    CHECK(sounds.voiceCount() == 1);
    f.effects.update(1.0f / 30);
    f.device.draws.clear();
    f.effects.draw(f.device, Mat4{1}, f.world.fullLighting());
    CHECK_FALSE(f.device.draws.empty());
    f.effects.clear();
    f.audio.close();
}

TEST_CASE("Temple wall projectile hits remove the mesh and collision through the scene dispatch",
          "[game][screens][player-attacks][walls][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("audio/COMMON.vbk");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    f.audio.open(root, &sounds, f.world.audio());
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    const auto& walls = f.world.walls();
    REQUIRE(walls.size() == 5);
    CHECK(walls.target(0, 0).pointNear({55, 3, -5}) == Vec3{55, 3, -10});
    REQUIRE(walls.wall(0).health == 25);
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    MissileLaunch launch;
    launch.position = {55, 3, -5};
    launch.velocity = Vec3{0, 0, -20};
    launch.spec = &spec;
    launch.damage = 10;
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.4f, f.players, f.targets);
    CHECK(walls.wall(0).health == 16);
    CHECK(f.world.collision().solid(walls.wall(0).object));
    CHECK(sounds.voiceCount() == 1);
    launch.damage = 17;
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.4f, f.players, f.targets);
    CHECK_FALSE(walls.standing(0));
    CHECK_FALSE(f.world.collision().solid(walls.wall(0).object));
    CHECK(sounds.voiceCount() == 2);
    // The subtype-42 death path has a sound, not an invented barrel explosion.
    CHECK(f.effects.count() == 0);
    f.fixtures.clear();
    f.audio.close();
}

TEST_CASE("arena cover is not an aim target but still intercepts thrown weapons",
          "[game][screens][player-attacks][target-assist][safe-rocks]") {
    const auto root = turboAssets();
    writeTextFile(root / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "itemInfos":[
      {"type":10,"subtype":41,"name":"ROCK","collisionType":1,
       "radius":2.3,"height":5,"hitPoints":40,"armor":10}],
      "itemInstances":[{"info":0,"position":[0,0,6],
       "params":[41,0,3,0,0,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    Fixture f;
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    ItemArchive missing;
    auto& rocks = f.fixtures.safeRocks();
    REQUIRE(rocks.bind(f.device, layout, missing));
    REQUIRE(rocks.standing(0));
    const auto& actor = f.players[0].actor;
    CHECK_FALSE(f.attacks.aim(actor, Vec3{0, 0, 1}, f.targets));

    // Aim eligibility must not remove the barrier from collision/damage targets.
    const MissileSpec spec;
    MissileLaunch launch;
    launch.spec = &spec;
    launch.owner = actor.player();
    launch.position = {0, 2.5f, 0};
    launch.velocity = {0, 0, 30};
    launch.damage = 20;
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.3f, f.players, f.targets);
    CHECK(f.arsenal.missiles().count() == 0);
    CHECK(rocks.rock(0).health == 110);
    CHECK(rocks.obstacles().size() == 1);
    CHECK(rocks.blocksBreath({0, 2, 0}, {0, 2, 12}));

    // An off-axis creature remains eligible despite the closer cover ahead.
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = {3, 0, 14};
    REQUIRE(enemies.spawn(spawn, {}));
    const auto aim = f.attacks.aim(actor, Vec3{0, 0, 1}, f.targets);
    REQUIRE(aim);
    CHECK(aim->x == Approx(3));
    CHECK(aim->z == Approx(14));
    f.fixtures.clear();
}

TEST_CASE("a blow on a secret wall tells of multiple hits; a swing passes the safe rocks by",
          "[game][screens][player-attacks][walls][melee][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELB6/WORLDS.PS2");
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto temple = catalog.byName("E1");
    REQUIRE(temple);
    REQUIRE(f.world.load(f.device, root, *temple));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    f.players[0].figure = PlayerFigure::load(f.device, root, f.players[0].actor.save(), false);
    REQUIRE(f.players[0].figure);
    std::vector<s32> helps;
    f.targets.fixtureEvents.help = [&](s32 id, usize player) {
        CHECK(player == 0);
        helps.push_back(id);
        return true;
    };
    // Wall 0 faces +z at z = -10 (see the projectile test above).
    const auto& walls = f.world.walls();
    REQUIRE(walls.size() == 5);
    const s32 health = walls.wall(0).health;
    PlayerActor& actor = f.players[0].actor;
    actor.place({55, walls.target(0, 0).base.y, -9});
    actor.turnTo(std::numbers::pi_v<f32>);
    CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Melee);
    f.attacks.melee(0, f.players, f.targets);
    CHECK(walls.wall(0).health < health);
    CHECK(helps == std::vector<s32>{HelpMessages::kSecretWalls});
    f.fixtures.clear();

    // The dragon's lair: a swing never picks a rock, so beside one the character throws.
    const auto lair = catalog.byName("B6");
    REQUIRE(lair);
    REQUIRE(f.world.load(f.device, root, *lair));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    const SafeRocks& rocks = f.fixtures.safeRocks();
    REQUIRE(rocks.size() > 0);
    REQUIRE(rocks.standing(0));
    const Obstacle& cover = rocks.rock(0).obstacle;
    actor.place(cover.centre + Vec3{0, 0, cover.cylinderRadius + 1});
    actor.turnTo(std::numbers::pi_v<f32>);
    CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Attack);
    const s32 cover0 = rocks.rock(0).health;
    f.attacks.melee(0, f.players, f.targets);
    CHECK(rocks.rock(0).health == cover0);
    REQUIRE(rocks.size() == 6);
    for (usize i = 0; i < rocks.size(); ++i) {
        CAPTURE(i);
        REQUIRE(rocks.standing(i));
        const Obstacle& barrier = rocks.rock(i).obstacle;
        actor.place(barrier.centre + Vec3{0, 0, barrier.cylinderRadius + 1});
        CHECK_FALSE(f.attacks.aim(actor, Vec3{0, 0, -1}, f.targets));
    }
    f.fixtures.clear();
}

TEST_CASE("a thrown weapon stops at a chest and does it no harm",
          "[game][screens][player-attacks][item-stops][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const Chests& chests = f.fixtures.chests();
    std::optional<usize> shut;
    for (usize i = 0; i < chests.size() && !shut; ++i) {
        if (chests.chest(i).shown && chests.chest(i).state == Chests::kShut) {
            shut = i;
        }
    }
    REQUIRE(shut.has_value());
    const Obstacle& box = chests.chest(*shut).box;
    const s32 inside = chests.chest(*shut).contents;
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    MissileLaunch launch;
    launch.position = box.centre + Vec3{0, 1, 4};
    launch.velocity = Vec3{0, 0, -20};
    launch.spec = &spec;
    launch.damage = 20;
    REQUIRE(f.arsenal.missiles().launch(launch));
    for (s32 frame = 0; frame < 10; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    }
    CHECK(f.arsenal.missiles().count() == 0); // it went no further
    CHECK(chests.chest(*shut).state == Chests::kShut);
    CHECK(chests.chest(*shut).contents == inside);
    CHECK_FALSE(chests.chest(*shut).gone);
    f.fixtures.clear();
}

TEST_CASE("a thrown weapon sets off a target on the wall, but gas does not",
          "[game][screens][player-attacks][triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC3/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("C3");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    const LevelTriggers& triggers = f.world.triggers();
    usize target = triggers.size();
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).shootable) {
            target = i;
        }
    }
    REQUIRE(target < triggers.size());
    const s32 required = triggers.trigger(target).minPlayers;
    REQUIRE(required > 1);
    REQUIRE(required <= 4);
    const bool eligible = GENERATE(false, true);
    f.world.setPlayerCount(eligible ? required : 1);
    const Vec3 spot = triggers.trigger(target).spot + Vec3{0, 1, 0};
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    const auto throwAt = [&](const Vec3& from, u32 flags) {
        MissileLaunch launch;
        launch.position = from;
        launch.velocity = glm::normalize(spot - from) * 20.0f;
        launch.spec = &spec;
        launch.damage = 10;
        launch.flags = flags;
        REQUIRE(f.arsenal.missiles().launch(launch));
        f.attacks.updateProjectiles(0.4f, f.players, f.targets);
    };
    // From whichever side is open to it.
    bool shot = false;
    for (const Vec3 side : {Vec3{0, 0, 4}, Vec3{0, 0, -4}, Vec3{4, 0, 0}, Vec3{-4, 0, 0}}) {
        throwAt(spot + side, Damage::kGas);
        CHECK_FALSE(triggers.trigger(target).shot); // a cloud sets nothing off
        throwAt(spot + side, 0);
        if (triggers.trigger(target).shot) {
            shot = true;
            break;
        }
    }
    CHECK(shot == eligible);
    f.fixtures.clear();
}

TEST_CASE("swarm healing credit is capped incoming damage before level and armor adjustments",
          "[player-attacks][healing-magic]") {
    Fixture f;
    EnemyScales scales;
    scales.health = 100;
    scales.playerLevel = 70;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, turboAssets(), nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .placed = true}, {});
    REQUIRE(id);
    EnemyHit hit;
    hit.player = 3;
    hit.level = 80;
    hit.damage = 20;
    const f32 before = enemies.healthOf(*id);
    CHECK(enemies.hurt(*id, hit) == 20);
    CHECK(enemies.healthOf(*id) == Approx(before - 40));
    hit.level = 60;
    CHECK(enemies.hurt(*id, hit) == 20);
    CHECK(enemies.healthOf(*id) == Approx(before - 58));
    hit.damage = 100000;
    CHECK(enemies.hurt(*id, hit) == Approx(before - 58));
    CHECK_FALSE(enemies.alive(*id));
    CHECK(enemies.hurt(*id, hit) == 0);
    CHECK(enemies.hurt(-1, hit) == 0);
}

TEST_CASE("a healing missile earns its raw credit before elemental amplification",
          "[player-attacks][healing-magic]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    auto& save = f.players[0].actor.save();
    save.progress().experience = levelExperience(80);
    save.progress().health = 100;
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, EnemyScales{.health = 100}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .position = {0, 0, 2}, .placed = true}, {});
    REQUIRE(id);
    const f32 before = enemies.healthOf(*id);
    MissileLaunch launch;
    launch.owner = 3;
    launch.position = {0, 1, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::of(0);
    launch.damage = 10;
    launch.flags = damage::kHeal | 1;
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.1f, f.players, f.targets);
    CHECK(enemies.healthOf(*id) == Approx(before - 15));
    CHECK(save.health() == 102); // round(10 * .18), not round(15 * .18)
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("from level 75 magic heals its caster by a share of the harm it does",
          "[game][screens][player-attacks][healing-magic]") {
    const auto root = turboAssets();
    for (const s32 level : {74, 80}) {
        CAPTURE(level);
        Fixture f;
        REQUIRE(f.classes.load(root / "pdata"));
        f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
        std::vector<s32> helps;
        f.targets.fixtureEvents.help = [&](s32 id, usize player) {
            CHECK(player == 0);
            helps.push_back(id);
            return true;
        };
        auto& save = f.players[0].actor.save();
        save.progress().experience = levelExperience(level);
        save.progress().health = 100;
        save.progress().inventory.addPotions(1, 1);
        EnemyScales scales;
        scales.health = 100;
        auto& enemies = f.opponents.enemies();
        enemies.open(f.device, root, nullptr, 8, scales, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.placed = true;
        spawn.position = Vec3{0, 0, 2};
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        const f32 before = enemies.healthOf(*id);
        f.attacks.usePotion(0, f.players);
        for (s32 frame = 0; frame < 60; ++frame) {
            f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
        }
        const f32 harm = before - enemies.healthOf(*id);
        REQUIRE(harm > 0.0f);
        const bool heals = level >= 75;
        // Swarm healing uses incoming damage before the unshielded element's 1.5 multiplier.
        const auto expected = static_cast<s32>(std::lround(harm / 1.5f * (0.1f + 0.016f * 5.0f)));
        CHECK(save.health() == (heals ? 100 + expected : 100));
        CHECK((std::ranges::find(helps, HelpMessages::kHealingMagic) != helps.end()) == heals);
        CHECK(std::ranges::find(helps, HelpMessages::kWastedMagic) == helps.end());
        f.attacks.clear();
        f.opponents.close();
    }
}

TEST_CASE("healing magic shares half with nearby living partners but never departed records",
          "[player-attacks][healing-magic][multiplayer]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    std::array<PlayerRuntime, 2> party;
    CharacterSave save;
    save.progress().experience = levelExperience(80);
    save.progress().health = 100;
    party[0].actor.spawn(3, save, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, save, nullptr, Vec3{1, 0, 0}, 0);
    bool shared = true;
    SECTION("nearby standing partner") {}
    SECTION("dying partner") {
        party[1].life = PlayerLife::Dying;
        shared = false;
    }
    SECTION("partner waiting in the tower") {
        party[1].life = PlayerLife::InTower;
        shared = false;
    }
    SECTION("departed partner") {
        party[1].departed = true;
        shared = false;
    }
    SECTION("distant partner") {
        party[1].actor.place(Vec3{1000, 0, 0});
        shared = false;
    }
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, party);
    EnemyScales scales;
    scales.health = 100;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 2};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const f32 before = enemies.healthOf(*id);
    party[0].actor.save().progress().inventory.addPotions(1, 1);
    f.attacks.usePotion(0, party);
    for (s32 frame = 0; frame < 60; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, party, f.targets);
    }
    const f32 harm = before - enemies.healthOf(*id);
    REQUIRE(harm > 0);
    const f32 given = harm / 1.5f * (0.1f + 0.016f * 5.0f);
    CHECK(party[0].actor.save().health() == 100 + static_cast<s32>(std::lround(given)));
    CHECK(party[1].actor.save().health() ==
          100 + (shared ? static_cast<s32>(std::lround(given * 0.5f)) : 0));
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("only a hit carrying DMG_HEAL feeds the healing: the shield does, a turbo strike not",
          "[game][screens][player-attacks][healing-magic][damage-types]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    std::vector<s32> helps;
    f.targets.fixtureEvents.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    auto& player = f.players[0];
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    auto& save = player.actor.save();
    save.progress().experience = levelExperience(80);
    save.progress().health = 100;
    save.progress().inventory.addPotions(1, 1);
    EnemyScales scales;
    scales.health = 100;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 5};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    // The warrior's turbo rows carry no DMG_HEAL: the strike harms and heals nothing.
    const f32 whole = enemies.healthOf(*id);
    player.turbo.add(100);
    for (s32 frame = 0; frame < 30; ++frame) {
        player.figure->animate(0, 2, 1.0f / 30,
                               frame == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        f.attacks.updateTurbo(0, 2, 1.0f / 30, f.players, [](s32, usize) {});
        f.attacks.updateStrikes(1.0f / 30, f.players, f.targets);
    }
    REQUIRE(enemies.healthOf(*id) < whole);
    CHECK(save.health() == 100);
    CHECK(std::ranges::find(helps, HelpMessages::kHealingMagic) == helps.end());
    // The potion shield's magic carries it from 25 (start_magic's mode 1 shares the flags).
    const f32 before = enemies.healthOf(*id);
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateShields(0.1f, f.players, f.targets);
    const f32 harm = before - enemies.healthOf(*id);
    REQUIRE(harm > 0.0f);
    const auto expected = static_cast<s32>(std::lround(25.0f * (0.1f + 0.016f * 5.0f)));
    CHECK(save.health() == 100 + expected);
    CHECK(std::ranges::find(helps, HelpMessages::kHealingMagic) != helps.end());
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("a potion shield of the caster's own colour harms a tenth more",
          "[game][screens][player-attacks][damage-types]") {
    const auto root = turboAssets();
    Fixture f;
    REQUIRE(f.classes.load(root / "pdata"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& save = f.players[0].actor.save();
    save.color = 0; // yellow: light, potion kind 3
    save.progress().inventory.addPotions(1, 1);
    save.progress().inventory.addPotions(3, 1);
    EnemyScales scales;
    scales.health = 100;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 3};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    // Twenty-five of magic (no armour off it), one and a half times for an element the
    // swarm has no shield against; the own colour's a tenth over that.
    const f32 whole = enemies.healthOf(*id);
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateShields(0.1f, f.players, f.targets);
    CHECK(whole - enemies.healthOf(*id) == Approx(25.0f * 1.1f * 1.5f));
    // The first shield waits half a second before harming again; a second, of the other
    // potion, harms at once.
    const f32 again = enemies.healthOf(*id);
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(f.attacks.shieldCount() == 2);
    f.attacks.updateShields(0.1f, f.players, f.targets);
    CHECK(again - enemies.healthOf(*id) == Approx(25.0f * 1.5f));
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("a caster whose magic strikes nothing is told not to waste it",
          "[game][screens][player-attacks][healing-magic]") {
    Fixture f;
    std::vector<s32> helps;
    f.targets.fixtureEvents.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    f.players[0].actor.save().progress().inventory.addPotions(1, 1);
    f.attacks.usePotion(0, f.players);
    for (s32 frame = 0; frame < 60; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    }
    CHECK(helps == std::vector<s32>{HelpMessages::kWastedMagic});
    // A bottle a blast broke is nobody's: nobody is told.
    helps.clear();
    f.attacks.shatterPotion(1, Vec3{0});
    for (s32 frame = 0; frame < 60; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    }
    CHECK(helps.empty());
}

TEST_CASE("player shields consume one potion and expire even without artwork",
          "[game][screens][player-attacks]") {
    Fixture f;
    auto& inventory = f.players[0].actor.save().progress().inventory;
    inventory.addPotions(2, 2);
    f.attacks.shieldPotion(5, f.players);
    REQUIRE(inventory.potions.size() == 2);
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(inventory.potions.size() == 1);
    REQUIRE(f.attacks.shieldCount() == 1);
    f.attacks.updateShields(2, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 1);
    f.attacks.updateShields(1, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("player shields stop following participants who have fallen or left",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.players[0].actor.save().progress().inventory.addPotions(1, 2);
    f.attacks.shieldPotion(0, f.players);
    f.players[0].life = PlayerLife::Dying;
    f.attacks.updateShields(0.1f, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
    f.players[0].life = PlayerLife::Standing;
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateShields(0.1f, {}, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
}

TEST_CASE("block presentation limits duration and cannot restart during its cooldown",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.attacks.showBlock(0, 2, 100, f.players);
    REQUIRE(f.players[0].blockLeft == 0);
    f.attacks.showBlock(0, 3, 1, f.players);
    REQUIRE(f.players[0].blockLeft == Approx(0.333f));
    f.attacks.showBlock(0, 3, 1000, f.players);
    REQUIRE(f.players[0].blockLeft == Approx(0.333f));
    f.players[0].blockLeft = 0;
    f.attacks.showBlock(0, 3, 1000, f.players);
    REQUIRE(f.players[0].blockLeft == 1);
}

TEST_CASE("ordinary melee and finishers preserve the weapon element while guard flashes do not",
          "[game][player-attacks][alpha-elemental-melee][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    const u32 element = GENERATE(0U, 1U, 2U, 3U, 4U);
    const auto blow =
        GENERATE(MeleeBlow::Plain, MeleeBlow::Heavy, MeleeBlow::Kick, MeleeBlow::Power);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    std::array<PlayerRuntime, 2> party;
    party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 2.5f}, 0);
    auto& inventory = party[0].actor.save().progress().inventory;
    if (element != 0) {
        inventory.addPowerup(powerup::kWeapon, element, 0, 60);
    }
    party[0].figure = PlayerFigure::load(f.device, root, party[0].actor.save(), false);
    REQUIRE(party[0].figure);
    const auto& animator = party[0].figure->animator();
    f.targets.multiplayer = MultiplayerMode::Hurt;
    f.targets.players = party;
    s32 contacts = 0;
    u32 extra = 0;
    f32 multiplier = 1;
    PlayerDeed first = PlayerDeed::Melee;
    switch (blow) {
    case MeleeBlow::Heavy:
        extra = EnemyHit::kKnockBack;
        multiplier = 2;
        first = PlayerDeed::MeleeSlow;
        break;
    case MeleeBlow::Kick: first = PlayerDeed::MeleeLow; break;
    case MeleeBlow::Power:
        extra = EnemyHit::kKnockDown;
        multiplier = 3;
        break;
    default: break;
    }
    // PlayerMotion +0x3824 begins with field_11C for all melee damage, then ORs
    // knockback/down for heavy/power swings. The guard flash is a separate path.
    f.targets.hurt = [&](usize index, f32 amount, HurtKind, const PlayerImpact& impact) {
        CHECK(index == 1);
        ++contacts;
        CHECK(impact.flags == (element | extra));
        CHECK(amount == Approx(PlayerMissiles::kLeastDamage * multiplier));
    };
    const auto advance = [&](PlayerDeed deed) {
        party[0].figure->animate(0, 2, 1.0f / 30, deed);
        if (animator.meleeBlow() == blow) {
            f.attacks.melee(0, party, f.targets);
        }
    };
    advance(first);
    if (blow == MeleeBlow::Power) {
        advance(PlayerDeed::MeleeSlow); // buffered strong press after the first quick swing
    }
    for (s32 frame = 0; frame < 120 && contacts == 0; ++frame) {
        advance(PlayerDeed::None);
    }
    REQUIRE(contacts == 1);
    // StartBlockFX (8009233C) always chooses FX_BLOCK; it never reads the
    // weapon's low-nibble element or creates a retaliatory elemental attack.
    f.attacks.showBlock(0, 10, 20, party);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "BLOCKFX");
    CHECK(contacts == 1);
    f.effects.clear();
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, party);
    auto& enemies = f.opponents.enemies();
    // Exercise both surviving and lethal enemy routes at this same completed
    // contact, including the finisher's elemental death burst and skin choice.
    for (const bool lethal : {false, true}) {
        CAPTURE(lethal);
        EnemyScales scales;
        scales.health = lethal ? 0.01f : 10.0f;
        enemies.open(f.device, root, nullptr, 4, scales, 7);
        REQUIRE(enemies.loadKind(13));
        EnemySpawn spawn;
        spawn.kind = 13;
        spawn.tier = 3;
        spawn.placed = true;
        spawn.position = {0, 0, 2.5f};
        REQUIRE(enemies.spawn(spawn, {}));
        f.attacks.melee(0, party, f.targets);
        const auto feedback = enemies.takeFeedback();
        REQUIRE(feedback.size() == 1);
        CHECK(feedback[0].close);
        CHECK(feedback[0].killed == lethal);
        CHECK(damage::element(feedback[0].flags) == element);
        CHECK(feedback[0].effect() == damage::hitEffect(element, lethal));
        constexpr std::array<std::string_view, 5> kSkins{"DEATHBLOOD", "DEATHFIRE", "DEATHELEC",
                                                         "DEATHLIGHT", "DEATHACID"};
        CHECK(feedback[0].deathSkin() == kSkins[element]);
        CHECK(contacts == 1); // An enemy target takes priority over player fallback.
    }
    f.attacks.clear();
    f.opponents.close();
    f.arsenal.clear();
    f.effects.clear();
}

TEST_CASE("player attacks clear transient state and safely ignore closed or missing figures",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.players[0].actor.save().progress().inventory.addPotions(1, 2);
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateTurbo(0, 2, 0.1f, f.players,
                          [](s32, usize) { FAIL("No figure, no turbo announcement"); });
    REQUIRE(f.attacks.strikes().count() == 0);
    f.attacks.clear();
    f.attacks.clear();
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(f.attacks.shieldCount() == 0);
    REQUIRE(f.players[0].actor.save().progress().inventory.potions.size() == 1);
}
TEST_CASE("close attacks resolve to melee while distant attacks still throw",
          "[game][screens][player-attacks][melee][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    Fixture f;
    f.players[0].figure = PlayerFigure::load(f.device, root, f.players[0].actor.save(), false);
    REQUIRE(f.players[0].figure);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = {0, 0, 2.5f};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    CHECK(f.attacks.attackDeed(f.players[0].actor, false, f.targets) == PlayerDeed::Melee);
    CHECK(f.attacks.attackDeed(f.players[0].actor, true, f.targets) == PlayerDeed::MeleeSlow);
    f.players[0].actor.place({0, 0, -20});
    CHECK(f.attacks.attackDeed(f.players[0].actor, false, f.targets) == PlayerDeed::Attack);
    CHECK(f.attacks.attackDeed(f.players[0].actor, true, f.targets) == PlayerDeed::StrongAttack);
    f.players[0].actor.place({0, 0, 0});
    auto& figure = *f.players[0].figure;
    bool contacted = false;
    for (s32 frame = 0; frame < 30 && !contacted; ++frame) {
        figure.animate(0, 2, 1.0f / 30, f.attacks.attackDeed(f.players[0].actor, false, f.targets));
        CHECK_FALSE(figure.animator().released());
        contacted = figure.animator().meleeStruck();
    }
    CHECK(contacted);
    enemies.close();
}

TEST_CASE("a halo wearer drains Death only while touching the nearest target ahead",
          "[game][screens][player-attacks][death][assets]") {
    const s32 tier = GENERATE(1, 2);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    Fixture f;
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    EnemyScales scales;
    scales.health = 20;
    enemies.open(f.device, test::deathArchive(), nullptr, 2, scales, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto death = enemies.spawn(
        {.kind = kDeathKind, .tier = tier, .position = {0, 0, 2}, .placed = true}, {});
    REQUIRE(death);
    PlayerActor& actor = f.players[0].actor;
    actor.turnTo(0.0f); // facing him
    // Without a halo there is no hold.
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets).has_value());
    actor.save().progress().inventory.addPowerup(powerup::kArmor, DeathRules::kProtection, 0, 60);
    const f32 full = enemies.healthOf(*death);
    const auto held = f.attacks.grabDeath(0, 2, true, f.players, f.targets);
    REQUIRE(held.has_value());
    CHECK(*held == enemies.positionOf(*death));
    CHECK(f.players[0].deathHeld == *death);
    CHECK(f.effects.count() == 1); // his drain, about the one holding him
    for (s32 frame = 0; frame < 9; ++frame) {
        REQUIRE(f.attacks.grabDeath(0, 2, true, f.players, f.targets).has_value());
    }
    CHECK(enemies.healthOf(*death) == full - 10); // a point a 30 Hz frame
    const auto cues = enemies.takeDeathEvents();
    CHECK(std::ranges::count_if(cues, [](const DeathEvent& cue) {
              return cue.kind == DeathEvent::Kind::Return;
          }) == 10);
    // Turned away, or not allowed, the hold is let go.
    actor.turnTo(std::numbers::pi_v<f32>);
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets).has_value());
    CHECK(f.players[0].deathHeld == -1);
    actor.turnTo(0.0f);
    REQUIRE(f.attacks.grabDeath(0, 2, true, f.players, f.targets).has_value());
    CHECK_FALSE(f.attacks.grabDeath(0, 2, false, f.players, f.targets).has_value());
    const f32 beforeEscape = enemies.healthOf(*death);
    actor.place({12, 0, -10});
    // At this range closest_enemy's cone is narrower than the near 0.707 cone.
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    CHECK(enemies.healthOf(*death) == beforeEscape);
    actor.place({0, -11, 0});
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    actor.place({0, 0, 0});
    REQUIRE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    const f32 beforeVerticalEscape = enemies.healthOf(*death);
    actor.place({0, -40, 0});
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    CHECK(enemies.healthOf(*death) == beforeVerticalEscape);
    CHECK(f.players[0].deathHeld == -1);
    CHECK(f.players[0].deathHeldEffect == 0);
    actor.place({0, 0, -40});
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    actor.place({0, 0, 0});
    const std::array views{EnemyView{.player = 0, .position = actor.position(), .antiDeath = true}};
    const Vec3 beforeRetreat = enemies.positionOf(*death);
    for (s32 frame = 0; frame < 45; ++frame) {
        enemies.update(2, 1.0f / 30, views);
    }
    REQUIRE(enemies.positionOf(*death).z > beforeRetreat.z);
    // A valid ranged target is not a valid drain contact, including after retreat.
    actor.place(enemies.positionOf(*death) - Vec3{0, 0, 10});
    const f32 beforeRetreatDrain = enemies.healthOf(*death);
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    CHECK(enemies.healthOf(*death) == beforeRetreatDrain);
    actor.place(enemies.positionOf(*death) - Vec3{0, 0, 2});
    REQUIRE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    CHECK(enemies.healthOf(*death) == beforeRetreatDrain - 1);
    actor.place({0, 0, -100});
    CHECK_FALSE(f.attacks.grabDeath(0, 2, true, f.players, f.targets));
    CHECK(enemies.healthOf(*death) == beforeRetreatDrain - 1);
    CHECK(f.players[0].deathHeld == -1);
    f.opponents.close();
}

TEST_CASE("pausing a halo hold silences its loops without advancing the drain",
          "[game][screens][player-attacks][death][pause]") {
    const auto root = test::scratchDirectory("halo-pause-audio");
    const std::array<test::NativeSoundSample, 1> samples{{{24000, std::vector<s16>(240, 4096)}}};
    test::writeNativeSoundBank(root / "audio/COMMON", R"({"sounds":[
        {"name":"S_DEATHDIE","duration":-1,"sequence":[{"sample":0,"loopStart":true,"loopBack":true}]},
        {"name":"S_DEATHSUCK","duration":-1,"sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}
    ]})",
                               samples);
    AudioMixer mixer(24000);
    SoundPlayer sounds(mixer);
    Fixture f;
    f.audio.open(root, &sounds, nullptr);
    auto& player = f.players[0];
    player.deathHeld = 1;
    player.deathHeldTicks = 1;
    player.deathHeldCry = f.audio.playNamed("S_DEATHDIE");
    player.deathHeldSuck = f.audio.playNamed("S_DEATHSUCK");
    const auto cry = player.deathHeldCry;
    const auto suck = player.deathHeldSuck;
    REQUIRE(sounds.isPlaying(cry));
    REQUIRE(sounds.isPlaying(suck));
    f.attacks.stopDeathSounds(f.players);
    CHECK_FALSE(sounds.isPlaying(cry));
    CHECK_FALSE(sounds.isPlaying(suck));
    CHECK(player.deathHeldCry == kNoSound);
    CHECK(player.deathHeldSuck == kNoSound);
    CHECK(player.deathHeld == 1);
    CHECK(player.deathHeldTicks == 1);
    f.audio.close();
}

TEST_CASE("fire and lightning shields harm the creature their bearer stands against",
          "[game][screens][player-attacks][shield][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    for (const u32 shield : {powerup::kFireShield, powerup::kLightningShield}) {
        CAPTURE(shield);
        Fixture f;
        REQUIRE(f.world.load(f.device, root, *level));
        REQUIRE(f.weapons.load(root / "WEAPONS"));
        f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
        f.players[0].figure = PlayerFigure::load(f.device, root, f.players[0].actor.save(), false);
        REQUIRE(f.players[0].figure);
        auto& enemies = f.opponents.enemies();
        EnemyScales scales;
        scales.health = 100.0f;
        enemies.open(f.device, root, nullptr, 4, scales, 7);
        REQUIRE(enemies.loadKind(kGruntKind));
        const auto id = enemies.spawn(
            {.kind = kGruntKind, .tier = 3, .position = {0, 0, 2.5f}, .placed = true}, {});
        REQUIRE(id);
        const f32 full = enemies.healthOf(*id);
        // Unshielded, standing against it harms nothing.
        f.attacks.updateArmour(1.0f / 30, f.players, f.targets);
        CHECK(enemies.healthOf(*id) == full);
        f.players[0].actor.save().progress().inventory.addPowerup(powerup::kArmor, shield, 0, 60);
        f.attacks.updateArmour(1.0f / 30, f.players, f.targets);
        const f32 once = enemies.healthOf(*id);
        CHECK(once < full);
        f.attacks.updateArmour(1.0f / 30, f.players, f.targets);
        if (shield == powerup::kFireShield) {
            CHECK(enemies.healthOf(*id) < once); // every frame, never waiting
            CHECK(f.effects.count() == 0);
        } else {
            CHECK(enemies.healthOf(*id) == once); // once a second
            CHECK(f.effects.count() == 1);        // the spark, from the shield to it
            for (s32 frame = 0; frame < 30; ++frame) {
                f.attacks.updateArmour(1.0f / 30, f.players, f.targets);
            }
            CHECK(enemies.healthOf(*id) < once);
        }
        // Out of reach, it harms nothing.
        f.players[0].actor.place({0, 0, -20});
        const f32 away = enemies.healthOf(*id);
        for (s32 frame = 0; frame < 40; ++frame) {
            f.attacks.updateArmour(1.0f / 30, f.players, f.targets);
        }
        CHECK(enemies.healthOf(*id) == away);
        f.attacks.clear();
        f.opponents.close();
    }
}

TEST_CASE("melee acquires ahead of the requested heading, within a swing or a step",
          "[game][screens][player-attacks][melee][alpha-auto-melee][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = {0, 0, 2.5f};
    REQUIRE(enemies.spawn(spawn, {}));
    PlayerActor& actor = f.players[0].actor;
    // An unrequested target behind the character does not turn a throw into melee.
    actor.place({0, 0, 5});
    actor.turnTo(0.0f);
    CHECK(f.attacks.meleeSense(actor, true, f.targets).range == MeleeRange::Beyond);
    CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Attack);
    CHECK(f.attacks.attackDeed(actor, true, f.targets) == PlayerDeed::StrongAttack);
    // Desired movement can acquire behind the body's current yaw, preserving
    // the 180-degree combo's melee_yaw while the attack restricts body turning.
    const Vec3 requested{0, 0, -1};
    const MeleeSense behind = f.attacks.meleeSense(actor, true, f.targets, requested);
    CHECK(behind.range == MeleeRange::Swing);
    CHECK(std::abs(behind.yaw) == Approx(std::numbers::pi_v<f32>).margin(0.01f));
    const PlayerDeed close = f.attacks.attackDeed(actor, false, f.targets, false, 0, requested);
    CHECK((close == PlayerDeed::Melee || close == PlayerDeed::MeleeLow));
    // Backing off: a step away, it is stepped to only while the stick moves.
    bool stepped = false;
    for (f32 z = 5.0f; z < 12.0f && !stepped; z += 0.1f) {
        actor.place({0, 0, z});
        const MeleeSense sense = f.attacks.meleeSense(actor, true, f.targets, requested);
        if (sense.range != MeleeRange::Step) {
            continue;
        }
        stepped = true;
        CHECK(f.attacks.attackDeed(actor, false, f.targets, false, 0, requested) ==
              PlayerDeed::Attack);
        CHECK(f.attacks.attackDeed(actor, false, f.targets, true, 0, requested) ==
              (sense.low ? PlayerDeed::Attack : PlayerDeed::Melee));
        CHECK(f.attacks.attackDeed(actor, true, f.targets, true, 0, requested) ==
              (sense.low ? PlayerDeed::StrongAttack : PlayerDeed::MeleeSlow));
        // Let go of, the attack reaches a unit less.
        CHECK(f.attacks.meleeSense(actor, false, f.targets, requested).range != MeleeRange::Swing);
    }
    CHECK(stepped);
    actor.place({0, 0, 20});
    CHECK(f.attacks.meleeSense(actor, true, f.targets, requested).range == MeleeRange::Beyond);
    enemies.close();
}

TEST_CASE("automatic melee requires a forward creature inside the unheld swing band",
          "[game][player-attacks][melee][alpha-auto-melee]") {
    const auto root = turboAssets();
    Fixture f;
    auto& actor = f.players[0].actor;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 3;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}));
    const auto bodies = enemies.targets();
    REQUIRE(bodies.size() == 1);
    const auto& target = bodies.front();
    const f32 boundary = actor.reach() + PlayerAttacks::kSwingReach;
    for (const f32 gap : {boundary - 0.25f, boundary, boundary + 0.25f}) {
        CAPTURE(gap, boundary);
        actor.place(target.base - Vec3{0, 0, target.radius + gap});
        CHECK(f.attacks.meleeSense(actor, true, f.targets).range == MeleeRange::Swing);
        CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) ==
              (gap < boundary ? PlayerDeed::AutoMelee : PlayerDeed::None));
    }
    actor.place(target.base - Vec3{0, 0, target.radius + boundary - 0.25f});
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, {0, 0, -1}) == PlayerDeed::None);
    const PlayerAttacks unbound;
    CHECK(unbound.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    auto& inventory = actor.save().progress().inventory;
    inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 3, 1);
    const Inventory before = inventory;
    CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::SuperShot);
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::AutoMelee);
    CHECK(inventory == before);
    CHECK(f.arsenal.missiles().count() == 0);
    enemies.close();
}

TEST_CASE("automatic melee selects live generators but never player fallback targets",
          "[game][player-attacks][melee][alpha-auto-melee]") {
    const auto root = turboAssets();
    Fixture f;
    auto& actor = f.players[0].actor;
    auto& enemies = f.opponents.enemies();
    auto& generators = f.opponents.generators();
    enemies.open(f.device, root, nullptr, 4, {}, 1);
    ItemInfo generator;
    generator.type = ItemInfo::kGenerator;
    generator.name = "BOSSGEN";
    generator.hitPoints = 500;
    generator.height = 6;
    generator.xSize = 2;
    generator.zSize = 2;
    REQUIRE(generators.placeBoss(f.device, generator, f.weapons, enemies, kGruntKind, Mat4{1},
                                 nullptr));
    const auto& box = generators.boxOf(0);
    actor.place(generators.positionOf(0) - Vec3{0, 0, std::max(box.halfAcross, box.halfAlong) + 1});
    REQUIRE(generators.standing(0));
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::AutoMelee);
    REQUIRE(generators.strike(0, 10000, 0));
    REQUIRE_FALSE(generators.standing(0));
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    std::array<PlayerRuntime, 1> other;
    other[0].actor.spawn(1, {}, nullptr, actor.position() + Vec3{0, 0, 2}, 0);
    f.targets.players = other;
    f.targets.multiplayer = MultiplayerMode::Hurt;
    REQUIRE(f.attacks.meleeSense(actor, true, f.targets).range == MeleeRange::Swing);
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    generators.clear();
    enemies.close();
}

TEST_CASE("automatic melee tests the chosen target rather than searching past a closer barrel",
          "[game][player-attacks][melee][alpha-auto-melee][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    std::optional<usize> barrel;
    for (usize i = 0; i < f.fixtures.barrels().size(); ++i) {
        if (f.fixtures.barrels().barrel(i).instance == 319) {
            barrel = i;
        }
    }
    REQUIRE(barrel);
    REQUIRE(f.fixtures.barrels().standing(*barrel));
    const auto& cask = f.fixtures.barrels().barrel(*barrel);
    auto& actor = f.players[0].actor;
    actor.place(cask.figure.position() - Vec3{0, 0, cask.radius + 0.5f});
    REQUIRE(f.attacks.meleeSense(actor, true, f.targets).low);
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = actor.position() + Vec3{0, 0, enemyKind(kGruntKind).radius + 1.5f};
    REQUIRE(enemies.spawn(spawn, {}));
    REQUIRE(f.attacks.meleeSense(actor, true, f.targets).low);
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::None);
    f.fixtures.clear();
    REQUIRE_FALSE(f.attacks.meleeSense(actor, true, f.targets).low);
    CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) == PlayerDeed::AutoMelee);
    enemies.close();
}

TEST_CASE("automatic melee only permits the native DRIDER and LICH boss exceptions",
          "[game][player-attacks][melee][alpha-auto-melee][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("CRITTER/DRIDER.WAD");
    test::assetOrSkip("CRITTER/DRAGON.WAD");
    Fixture f;
    auto& bosses = f.opponents.bosses();
    auto& actor = f.players[0].actor;
    for (const s32 kind : {34, 37, 41}) {
        CAPTURE(kind);
        bosses.open(f.device, root, nullptr, {}, 'G');
        REQUIRE(bosses.spawn(kind, {}, 0));
        bosses.wake();
        const auto bodies = bosses.targets();
        REQUIRE_FALSE(bodies.empty());
        bool checked = false;
        for (const auto& target : bodies) {
            actor.place(target.base - Vec3{0, 0, target.radius + 0.5f});
            if (f.attacks.meleeSense(actor, false, f.targets).range != MeleeRange::Swing) {
                continue;
            }
            checked = true;
            CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) ==
                  (kind == 34 ? PlayerDeed::None : PlayerDeed::AutoMelee));
        }
        CHECK(checked);
        bosses.close();
    }
}

TEST_CASE("a forward melee contact cannot be stolen by a nearer barrel behind the player",
          "[game][player-attacks][melee][alpha-auto-melee][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    std::optional<usize> barrel;
    for (usize i = 0; i < f.fixtures.barrels().size(); ++i) {
        if (f.fixtures.barrels().barrel(i).instance == 319) {
            barrel = i;
        }
    }
    REQUIRE(barrel);
    REQUIRE(f.fixtures.barrels().standing(*barrel));
    const auto& cask = f.fixtures.barrels().barrel(*barrel);
    const s32 barrelHealth = cask.health;
    auto& player = f.players[0];
    player.actor.place(cask.figure.position() + Vec3{0, 0, cask.radius + 0.5f});
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = player.actor.position() + Vec3{0, 0, enemyKind(kGruntKind).radius + 1.5f};
    const auto enemy = enemies.spawn(spawn, {});
    REQUIRE(enemy);
    const f32 enemyHealth = enemies.healthOf(*enemy);
    REQUIRE(f.attacks.automaticMeleeDeed(player.actor, f.targets, {0, 0, 1}) ==
            PlayerDeed::AutoMelee);
    player.figure->setMelee(f.attacks.meleeSense(player.actor, true, f.targets));
    for (s32 frame = 0; frame < 90 && !player.figure->animator().meleeStruck(); ++frame) {
        player.figure->animate(0, 2, 1.0f / 30, frame == 0 ? PlayerDeed::Melee : PlayerDeed::None);
    }
    REQUIRE(player.figure->animator().meleeStruck());
    player.meleeFacing = Vec3{0, 0, 1};
    f.attacks.melee(0, f.players, f.targets);
    CHECK(enemies.healthOf(*enemy) < enemyHealth);
    CHECK(cask.health == barrelHealth);
    // The body has not turned, but a requested backward combo uses its own
    // heading at contact rather than being hard-wired to the body's facing.
    player.meleeFacing = Vec3{0, 0, -1};
    f.attacks.melee(0, f.players, f.targets);
    CHECK(cask.health < barrelHealth);
    f.opponents.close();
}

TEST_CASE("native player width governs melee bands and the anklebiter low threshold",
          "[game][player-attacks][melee][alpha-melee-flow][alpha-auto-melee][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/MAG/ANIM.PS2");
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    Fixture f;
    REQUIRE(f.classes.load(root / "PDATA"));
    auto& enemies = f.opponents.enemies();
    for (const s32 kind : {12, 13}) {
        enemies.open(f.device, root, nullptr, 4, {}, 7);
        REQUIRE(enemies.loadKind(kind));
        EnemySpawn spawn;
        spawn.kind = kind;
        spawn.tier = 3;
        spawn.placed = true;
        REQUIRE(enemies.spawn(spawn, {}));
        const auto bodies = enemies.targets();
        REQUIRE(bodies.size() == 1);
        const auto& target = bodies.front();
        for (s32 character = 0; character < kStartingClassCount; ++character) {
            const auto* stats = f.classes.stats(character);
            REQUIRE(stats);
            CharacterSave save;
            save.character = character;
            auto& actor = f.players[0].actor;
            actor.spawn(0, save, stats, {}, 0);
            CAPTURE(kind, character, stats->width, target.radius);
            REQUIRE(actor.reach() == Approx(stats->width));
            actor.place(target.base - Vec3{0, 0, target.radius + stats->width + 0.75f});
            CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) ==
                  (kind == 12 ? PlayerDeed::AutoMeleeLow : PlayerDeed::AutoMelee));
            // PlayerMotion reads col_radius (+0x850), the full PDAT width;
            // distance already excludes the target's own radius. The held
            // swing boundary is width+1+1 and the low boundary is width+2.
            const f32 boundary = stats->width + 2;
            actor.place(target.base - Vec3{0, 0, target.radius + boundary - 0.1f});
            const auto swing = f.attacks.meleeSense(actor, true, f.targets);
            CHECK(swing.range == MeleeRange::Swing);
            CHECK(swing.low == (kind == 12));
            CHECK(f.attacks.attackDeed(actor, false, f.targets, true) ==
                  (kind == 12 ? PlayerDeed::MeleeLow : PlayerDeed::Melee));
            CHECK(f.attacks.attackDeed(actor, true, f.targets, true) ==
                  (kind == 12 ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow));
            CHECK(f.attacks.meleeSense(actor, false, f.targets).range == MeleeRange::Step);
            CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) ==
                  PlayerDeed::None);

            actor.place(target.base - Vec3{0, 0, target.radius + boundary + 0.1f});
            const auto step = f.attacks.meleeSense(actor, true, f.targets);
            CHECK(step.range == MeleeRange::Step);
            CHECK_FALSE(step.low); // low classification has no held-button extension
            CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Attack);
            CHECK(f.attacks.attackDeed(actor, false, f.targets, true) == PlayerDeed::Melee);

            actor.place(target.base - Vec3{0, 0, target.radius + boundary + 1.1f});
            CHECK(f.attacks.meleeSense(actor, true, f.targets).range == MeleeRange::Beyond);
            CHECK(f.attacks.attackDeed(actor, true, f.targets, true) == PlayerDeed::StrongAttack);
        }
    }
    enemies.close();
}

TEST_CASE("a completed low melee contact reaches the full native player width",
          "[game][player-attacks][melee][alpha-melee-flow][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/MAG/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    Fixture f;
    REQUIRE(f.classes.load(root / "PDATA"));
    const auto* stats = f.classes.stats(0);
    REQUIRE(stats);
    auto& player = f.players[0];
    player.actor.spawn(0, {}, stats, {}, 0);
    player.figure = PlayerFigure::load(f.device, root, player.actor.save(), false);
    REQUIRE(player.figure);
    player.figure->setMelee({MeleeRange::Swing, true, 0});
    player.figure->animate(0, 2, 1.0f / 30, PlayerDeed::MeleeLow);
    for (s32 frame = 0; frame < 120 && !player.figure->animator().meleeStruck(); ++frame) {
        player.figure->animate(0, 2, 1.0f / 30);
    }
    REQUIRE(player.figure->animator().meleeBlow() == MeleeBlow::Kick);
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(12));
    EnemySpawn spawn;
    spawn.kind = 12;
    spawn.tier = 3;
    spawn.placed = true;
    const auto enemy = enemies.spawn(spawn, {});
    REQUIRE(enemy);
    const auto bodies = enemies.targets();
    REQUIRE(bodies.size() == 1);
    const auto& target = bodies.front();
    const f32 boundary = stats->width + 2;
    const f32 health = enemies.healthOf(*enemy);
    player.actor.place(target.base - Vec3{0, 0, target.radius + boundary + 0.1f});
    f.attacks.melee(0, f.players, f.targets);
    CHECK(enemies.healthOf(*enemy) == health);
    player.actor.place(target.base - Vec3{0, 0, target.radius + boundary - 0.1f});
    f.attacks.melee(0, f.players, f.targets);
    CHECK(enemies.healthOf(*enemy) < health);
    const auto feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    CHECK(feedback.front().close);
    CHECK((feedback.front().flags & EnemyHit::kKnockDown) != 0);
    f.attacks.clear();
    f.opponents.close();
}

TEST_CASE("generals never select low melee from their small collision parts",
          "[game][player-attacks][alpha-combat][alpha-general-melee][alpha-auto-melee][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GENERAL/LEVELG/ANIM.PS2");
    Fixture f;
    auto& critters = f.opponents.critters();
    critters.open(f.device, root, nullptr, {}, 'G');
    const auto id = critters.spawnGeneral({0, 0, 5}, 0);
    REQUIRE(id);
    for (const f32 scale : {1.0f, 0.3f}) {
        CAPTURE(scale);
        critters.resize(*id, scale);
        const auto targets = critters.targets();
        REQUIRE_FALSE(targets.empty());
        bool checked = false;
        for (const auto& target : targets) {
            if (target.height > PlayerAttacks::kLowEnemy) {
                continue;
            }
            checked = true;
            auto& actor = f.players[0].actor;
            actor.place(target.base - Vec3{0, 0, target.radius + actor.radius() + 0.5f});
            const auto sense = f.attacks.meleeSense(actor, true, f.targets);
            REQUIRE(sense.range == MeleeRange::Swing);
            // PlayerMotion's critter branch sets creature bit0x10, never low bit2.
            CHECK_FALSE(sense.low);
            CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Melee);
            CHECK(f.attacks.automaticMeleeDeed(actor, f.targets, actor.facing()) ==
                  PlayerDeed::AutoMelee);
        }
        CHECK(checked);
    }
    critters.close();
}

TEST_CASE("a melee contact routes damage sound and impact once through level opponents",
          "[game][screens][player-attacks][melee][enemy-feedback][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::assetOrSkip("audio/TOWN.vbk");
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const LevelAudioInfo info{.bank = "TOWN", .stream = {}};
    f.audio.open(root, &sounds, &info);
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    // Keep the real level's sound roster, but place this melee fixture's actors
    // without a floor constraint so its geometry does not steer the contact.
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(13));
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = {0, 0, 2.5f};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    f.players[0].figure = PlayerFigure::load(f.device, root, f.players[0].actor.save(), false);
    REQUIRE(f.players[0].figure);
    const f32 health = enemies.healthOf(*id);
    auto& figure = *f.players[0].figure;
    s32 contacts = 0;
    for (s32 frame = 0; frame < 30 && contacts == 0; ++frame) {
        figure.animate(0, 2, 1.0f / 30, f.attacks.attackDeed(f.players[0].actor, false, f.targets));
        if (figure.animator().meleeStruck()) {
            f.attacks.melee(0, f.players, f.targets);
            ++contacts;
        }
    }
    REQUIRE(contacts == 1);
    CHECK(enemies.healthOf(*id) < health);
    // The blow counts one towards the narrator's praise, when the body is not a short one.
    const auto bodies = enemies.targets();
    const auto body =
        std::ranges::find_if(bodies, [&](const MissileTarget& target) { return target.id == *id; });
    REQUIRE(body != bodies.end());
    CHECK(f.players[0].streak.count() == (body->height > PlayerAttacks::kLowEnemy ? 1 : 0));
    const f32 afterHit = enemies.healthOf(*id);
    f.players[0].actor.place({0, 0, -20});
    f.attacks.melee(0, f.players, f.targets);
    CHECK(enemies.healthOf(*id) == afterHit); // contact rechecks reach, not the wind-up's target
    f.players[0].actor.place({0, 0, 0});
    s32 awards = 0;
    LevelOpponents::Events events;
    events.levels = [] {};
    events.award = [&](s32 player, s32, bool) {
        CHECK(player == 3);
        ++awards;
    };
    f.opponents.settleRewards(f.players, events);
    CHECK(awards == 1);
    CHECK(sounds.voiceCount() == 1);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "BLOODFX1");
    CHECK(f.effects.effect(0).tint.a == 96);
    f.opponents.settleRewards(f.players, events);
    CHECK(awards == 1);
    CHECK(sounds.voiceCount() == 1);
    f.opponents.strikeEnemy(*id, 1000, 0, {0, 0, 1}, 3, f.players, true);
    f.opponents.settleRewards(f.players, events);
    CHECK(awards == 2);
    CHECK(sounds.voiceCount() == 2);
    REQUIRE(f.effects.count() == 2);
    CHECK(f.effects.effect(1).name == "BLOODFX2");
    f.opponents.close();
    CHECK(f.effects.count() == 0);
    f.audio.close();
}
TEST_CASE("potion shields harm enemies behind the bearer and stop at expiration",
          "[game][items][player-attacks][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    EnemyScales scales;
    scales.health = 10;
    enemies.open(f.device, root, nullptr, 4, scales, 7);
    REQUIRE(enemies.loadKind(13));
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = {0, 0, -2};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    f.players[0].actor.save().progress().inventory.addPotions(1, 1);
    const auto before = enemies.healthOf(*id);
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateShields(0.01f, f.players, f.targets);
    CHECK(enemies.healthOf(*id) < before);
    const auto after = enemies.healthOf(*id);
    f.attacks.updateShields(0.1f, f.players, f.targets);
    CHECK(enemies.healthOf(*id) == after);
    f.attacks.updateShields(4, f.players, f.targets);
    CHECK(enemies.healthOf(*id) == after);
    f.attacks.clear();
    f.opponents.close();
}
TEST_CASE("weapon item flags survive the flight and produce elemental enemy feedback",
          "[game][items][player-attacks][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(13));
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = {0, 0, 2};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const auto before = enemies.healthOf(*id);
    MissileLaunch launch;
    launch.owner = 3;
    launch.position = {0, 1, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::of(0);
    launch.damage = 10;
    launch.flags = 1 | EnemyHit::kKnockDown;
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.1f, f.players, f.targets);
    CHECK(enemies.healthOf(*id) == Approx(before - (10 - enemyKind(13).armor) * 1.5f));
    const auto feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    CHECK(feedback[0].flags == launch.flags);
    f.attacks.clear();
    f.opponents.close();
}
TEST_CASE("player projectiles sever the contacted Chimera head through encounter hit routing",
          "[game][screens][player-attacks][chimera][assets]") {
    const auto root = test::assetOrSkip("CRITTER/CHIMERA.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/CHIMERA/ANIM.PS2");
    Fixture f;
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& bosses = f.opponents.bosses();
    bosses.open(f.device, root, nullptr, {}, 'A');
    REQUIRE(bosses.spawn(35, Vec3{0}, 0));
    bosses.wake(); // asleep, it would take nothing
    const auto targets = bosses.targets();
    const auto lion = std::ranges::find(targets, 2, &MissileTarget::id);
    REQUIRE(lion != targets.end());
    const Vec3 centre = lion->base + Vec3{0, lion->height * 0.5f, 0};
    const f32 health = bosses.view().health;
    MissileLaunch launch;
    launch.owner = 3;
    launch.position = centre + Vec3{0, 0, 10};
    launch.velocity = Vec3{0, 0, -60};
    launch.spec = &MissileSpec::of(0);
    launch.damage = 2000;
    REQUIRE(f.arsenal.missiles().launch(launch));
    for (s32 step = 0; step < 20 && f.arsenal.missiles().count() > 0; ++step) {
        f.attacks.updateProjectiles(1.0f / 60.0f, f.players, f.targets);
    }
    REQUIRE(f.arsenal.missiles().count() == 0);
    const auto surviving = bosses.targets();
    REQUIRE(std::ranges::none_of(surviving, [](const auto& target) { return target.id == 2; }));
    REQUIRE(std::ranges::any_of(surviving, [](const auto& target) { return target.id == 1; }));
    REQUIRE(bosses.view().health == health);
    f.attacks.clear();
    f.opponents.close();
}
TEST_CASE("explosions shatter world potions into ownerless magic without consuming inventory",
          "[game][screens][player-attacks][shattered-potion][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    EnemyScales scales;
    scales.health = 10;
    auto& enemies = f.opponents.enemies();
    enemies.open(f.device, root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(13));
    const Vec3 origin{10000, 0, 10000};
    const usize bottle = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "POT_BLU", origin));
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = origin + Vec3{0, 0, 2};
    const auto enemy = enemies.spawn(spawn, {});
    REQUIRE(enemy);
    spawn.position = origin + Vec3{0, 0, 40};
    const auto farEnemy = enemies.spawn(spawn, {});
    REQUIRE(farEnemy);
    const f32 before = enemies.healthOf(*enemy);
    const f32 farBefore = enemies.healthOf(*farEnemy);
    auto& inventory = f.players[0].actor.save().progress().inventory;
    inventory.addPotions(1, 1);
    usize releases = 0;
    f.targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    f.targets.fixtureEvents.help = [](s32, usize) -> bool {
        FAIL("A bottle is not destroyed food");
    };
    f.targets.fixtureEvents.shatterPotion = [&](s32 kind, const Vec3& position) {
        CHECK(kind == 2);
        CHECK(position == origin);
        ++releases;
        f.attacks.shatterPotion(kind, position);
    };
    f.fixtures.blast(origin, 12, 1, f.players, f.targets.fixtureEvents);
    REQUIRE(releases == 1);
    REQUIRE(f.world.placedItems().item(bottle).taken);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "MP_ELEC");
    CHECK(f.effects.effect(0).scale == Approx(0.5f)); // radius power 16 / 32
    CHECK_FALSE(inventory.potions.empty());
    f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    const f32 after = enemies.healthOf(*enemy);
    REQUIRE(after < before);
    const auto& sequence = f.effects.effect(0).tree->sequences[0];
    AnimationPlayer wave;
    wave.start(sequence, 0);
    const f32 duration = wave.secondsPerFrame() * static_cast<f32>(sequence.frames);
    const f32 phase = 1.0f - (1.0f / 30) / duration;
    // Magic bypasses armor; an unshielded enemy takes the retail 1.5 elemental multiplier.
    CHECK(before - after == Approx(32 * 1.5f * (phase - 0.33f) * 1.5f));
    CHECK(enemies.takeLosses().empty()); // No player gets experience for ownerless magic.
    for (s32 frame = 0; frame < 120; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
        f.effects.update(1.0f / 30);
        f.effects.draw(f.device, Mat4{1}, f.world.fullLighting());
    }
    CHECK(enemies.healthOf(*enemy) == after); // no repeated damage/stunlock
    CHECK(enemies.healthOf(*farEnemy) == farBefore);
    REQUIRE_FALSE(f.device.draws.empty());
    f.fixtures.blast(origin, 12, 50, f.players, f.targets.fixtureEvents);
    CHECK(releases == 1);
    f.attacks.clear();
    f.fixtures.clear();
    f.opponents.close();
}

TEST_CASE("world bottles detonate on weapon shots but ignore companion shots",
          "[game][screens][player-attacks][shot-potion][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    const Vec3 origin{10000, 0, 10000};
    const usize bottle = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "POT_BLU", origin));
    const std::vector<usize> lying = f.world.placedItems().shootablePotions();
    REQUIRE(std::ranges::find(lying, bottle) != lying.end());
    std::vector<s32> helps;
    f.targets.fixtureEvents.help = [&](s32 id, usize player) {
        CHECK(player == 0);
        helps.push_back(id);
        return true;
    };
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    MissileLaunch launch;
    launch.owner = f.players[0].actor.player();
    launch.position = origin + Vec3{0, 0.5f, -6};
    launch.velocity = Vec3{0, 0, 20};
    launch.spec = &spec;
    launch.damage = 10;
    SECTION("player weapon detonates both the bottle and the owner's magic") {}
    SECTION("companion projectile passes through without consuming the bottle") {
        launch.breaksPotions = false;
    }
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.5f, f.players, f.targets);
    CHECK(f.world.placedItems().item(bottle).taken == launch.breaksPotions);
    // The bottle's own blue magic and the thrower's, the thrower told shooting does less.
    usize bursts = 0;
    for (usize i = 0; i < f.effects.count(); ++i) {
        bursts += f.effects.effect(i).name == "MP_ELEC" ? 1 : 0;
    }
    if (launch.breaksPotions) {
        CHECK(bursts == 2);
        CHECK(helps == std::vector<s32>{HelpMessages::kShotMagic});
        CHECK(f.world.placedItems().shootablePotions().empty());
    } else {
        CHECK(bursts == 0);
        CHECK(helps.empty());
        const auto remaining = f.world.placedItems().shootablePotions();
        CHECK(std::ranges::find(remaining, bottle) != remaining.end());
        REQUIRE(f.arsenal.missiles().count() == 1);
        CHECK(f.arsenal.missiles().missile(0).position.z > origin.z);
    }
    f.attacks.clear();
    f.fixtures.clear();
}

TEST_CASE("potion magic leaves the plain, exploding and gas barrels alone but breaks one that "
          "holds something",
          "[game][screens][player-attacks][magic-immunity][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.targets.fixtureEvents.help = [](s32, usize) { return true; };
    f.targets.fixtureEvents.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    f.targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    const Breakables& barrels = f.fixtures.barrels();
    for (const auto kind : {BreakableStrike::Kind::Plain, BreakableStrike::Kind::Exploding,
                            BreakableStrike::Kind::Poison, BreakableStrike::Kind::Holding}) {
        std::optional<usize> cask;
        for (usize i = 0; i < barrels.size() && !cask; ++i) {
            if (barrels.standing(i) && barrels.barrel(i).kind == kind) {
                cask = i;
            }
        }
        if (!cask.has_value()) {
            continue;
        }
        CAPTURE(kind);
        f.attacks.shatterPotion(1, barrels.barrel(*cask).figure.position());
        for (s32 frame = 0; frame < 60; ++frame) { // the wave is spent well within this
            f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
        }
        CHECK(barrels.standing(*cask) == (kind != BreakableStrike::Kind::Holding));
    }
    f.attacks.clear();
    f.fixtures.clear();
}

/** A level loaded for the class perks, with a caster of `code` at `level` whose potions go
 * off where they stand. */
struct PerkLevel : Fixture {
    std::vector<s32> helps;

    PerkLevel(const std::filesystem::path& root, std::string_view name, std::string_view code,
              s32 level, s32 party = 1) {
        LevelCatalog catalog;
        REQUIRE(catalog.load(root));
        REQUIRE(world.load(device, root, *catalog.byName(name)));
        REQUIRE(weapons.load(root / "WEAPONS"));
        fixtures.bind({device, world, weapons, effects, audio, 1});
        fixtures.setPlayerCount(party);
        auto& save = players[0].actor.save();
        save.character = *classIndexOf(code);
        save.progress().experience = levelExperience(level);
        targets.fixtureEvents.help = [this](s32 id, usize player) {
            CHECK(player == 0);
            helps.push_back(id);
            return true;
        };
        targets.fixtureEvents.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
        targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    }
    PerkLevel(const PerkLevel&) = delete;
    PerkLevel& operator=(const PerkLevel&) = delete;
    PerkLevel(PerkLevel&&) = delete;
    PerkLevel& operator=(PerkLevel&&) = delete;
    ~PerkLevel() {
        attacks.clear();
        fixtures.clear();
    }

    /** Drinks a potion standing beside `where` and lets its wave run out. */
    void castBeside(const Vec3& where) {
        players[0].actor.place(where + Vec3{0, 0, 1});
        players[0].actor.save().progress().inventory.addPotions(1, 1);
        attacks.usePotion(0, players);
        for (s32 frame = 0; frame < 60; ++frame) {
            attacks.updateProjectiles(1.0f / 30, players, targets);
        }
    }
    bool taught(MagicPerkDeed deed) const {
        return std::ranges::find(helps, HelpMessages::kFirstMagicPerk + static_cast<s32>(deed)) !=
               helps.end();
    }
    std::string_view recordName(s32 record) const {
        return record >= 0
                   ? std::string_view{world.layout().itemInfos()[static_cast<usize>(record)].name}
                   : std::string_view{};
    }
};

usize firstShownTrap(const PerkLevel& f) {
    const Traps& traps = f.fixtures.traps();
    usize trap = 0;
    while (trap < traps.size() && !traps.trap(trap).shown) {
        ++trap;
    }
    REQUIRE(trap < traps.size());
    return trap;
}

std::filesystem::path perkRoot() {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    return root;
}

TEST_CASE("the warriors' magic turns the junk in a gold chest into silver, and from 50 gold",
          "[game][screens][player-attacks][magic-perks][assets]") {
    const auto root = perkRoot();
    for (const auto& [level, treasure, deed] :
         {std::tuple{30, "TREAS_SILVER", MagicPerkDeed::JunkToSilver},
          std::tuple{50, "TREAS_GOLD", MagicPerkDeed::JunkToGold}}) {
        CAPTURE(level);
        PerkLevel f(root, "G1", "WAR", level, 4);
        const Chests& chests = f.fixtures.chests();
        std::optional<usize> gold;
        for (usize i = 0; i < chests.size() && !gold; ++i) {
            if (chests.chest(i).shown && chests.chest(i).subtype == Chests::kGoldChest &&
                f.recordName(chests.chest(i).contents) == "TREAS_JUNK") {
                gold = i;
            }
        }
        REQUIRE(gold.has_value());
        // Junk lying in the open turns as well, to the same treasure.
        const Vec3 beside = chests.chest(*gold).figure.position();
        REQUIRE(f.world.placeItem(f.device, "TREAS_JUNK", beside + Vec3{0, 0, 2}));
        const usize junk = f.world.placedItems().size() - 1;
        f.castBeside(beside);
        CHECK(f.world.placedItems().item(junk).name == treasure);
        CHECK(f.recordName(chests.chest(*gold).contents) == treasure);
        CHECK(f.taught(deed));
        CHECK(chests.chest(*gold).figure.hasFigure());
    }
}

TEST_CASE("the wizards' magic cleanses spoiled fruit, and from 50 spoiled meat, in barrels",
          "[game][screens][player-attacks][magic-perks][assets]") {
    const auto root = perkRoot();
    for (const s32 level : {30, 50}) {
        CAPTURE(level);
        // G1's barrel of fruit shows to one player alone, its barrel of meat to three.
        for (const auto& [spoiled, cleansed, deed, party] :
             {std::tuple{"GAPPLE", "APPLE", MagicPerkDeed::CleanseFruit, 1},
              std::tuple{"BADMEAT", "CHICKEN", MagicPerkDeed::CleanseMeat, 4}}) {
            CAPTURE(spoiled);
            PerkLevel f(root, "G1", "WIZ", level, party);
            const Breakables& barrels = f.fixtures.barrels();
            std::optional<usize> cask;
            for (usize i = 0; i < barrels.size() && !cask; ++i) {
                if (barrels.standing(i) && f.recordName(barrels.barrel(i).contents) == spoiled) {
                    cask = i;
                }
            }
            REQUIRE(cask.has_value());
            f.castBeside(barrels.barrel(*cask).figure.position());
            const bool cleanses = level >= 50 || deed == MagicPerkDeed::CleanseFruit;
            CHECK((f.recordName(barrels.barrel(*cask).contents) == cleansed) == cleanses);
            CHECK(f.taught(deed) == cleanses);
        }
    }
    // Nobody else's magic touches food.
    PerkLevel f(root, "G1", "WAR", 50);
    const Breakables& barrels = f.fixtures.barrels();
    for (usize i = 0; i < barrels.size(); ++i) {
        if (barrels.standing(i) && f.recordName(barrels.barrel(i).contents) == "GAPPLE") {
            f.castBeside(barrels.barrel(i).figure.position());
            CHECK(f.recordName(barrels.barrel(i).contents) == "GAPPLE");
            break;
        }
    }
}

TEST_CASE("the valkyries' magic stops traps, and from 50 leaves them disarmed",
          "[game][screens][player-attacks][magic-perks][assets]") {
    const auto root = perkRoot();
    {
        PerkLevel f(root, "G1", "VAL", 30);
        const usize trap = firstShownTrap(f);
        f.castBeside(f.fixtures.traps().trap(trap).figure.position());
        CHECK(f.taught(MagicPerkDeed::StopTrap));
        CHECK_FALSE(f.fixtures.traps().armed(trap));
        CHECK_FALSE(f.fixtures.traps().trap(trap).disarmed);
    }
    PerkLevel f(root, "G1", "VAL", 50);
    const usize trap = firstShownTrap(f);
    f.castBeside(f.fixtures.traps().trap(trap).figure.position());
    CHECK(f.taught(MagicPerkDeed::DestroyTrap));
    CHECK(f.fixtures.traps().trap(trap).disarmed);
    CHECK_FALSE(f.fixtures.traps().trap(trap).gone); // LEVELG has its _D figure
    CHECK(f.fixtures.traps().trap(trap).figure.hasFigure());
}

TEST_CASE("the archers' magic shows up secret walls, and from 50 brings them down",
          "[game][screens][player-attacks][magic-perks][walls][assets]") {
    const auto root = perkRoot();
    test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2");
    {
        PerkLevel f(root, "E1", "ARC", 30);
        const auto& walls = f.world.walls();
        REQUIRE(walls.size() > 0);
        f.castBeside(Vec3{55, walls.target(0, 0).base.y, -8});
        CHECK(f.taught(MagicPerkDeed::RevealWall));
        CHECK(walls.standing(0));
        CHECK(walls.wall(0).blinks == DestructibleWalls::kRevealBlinks);
    }
    PerkLevel f(root, "E1", "ARC", 60);
    const auto& walls = f.world.walls();
    f.castBeside(Vec3{55, walls.target(0, 0).base.y, -8});
    CHECK(f.taught(MagicPerkDeed::DestroyWall));
    CHECK_FALSE(walls.standing(0));
}

TEST_CASE("a potion shield carries the perk too, and spares the plain barrels",
          "[game][screens][player-attacks][magic-perks][assets]") {
    const auto root = perkRoot();
    PerkLevel f(root, "G1", "VAL", 30);
    const usize trap = firstShownTrap(f);
    f.players[0].actor.place(f.fixtures.traps().trap(trap).figure.position() + Vec3{0, 0, 1});
    f.players[0].actor.save().progress().inventory.addPotions(1, 1);
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(f.attacks.shieldCount() == 1);
    for (s32 frame = 0; frame < 30; ++frame) {
        f.attacks.updateShields(1.0f / 30, f.players, f.targets);
    }
    CHECK(f.taught(MagicPerkDeed::StopTrap));
    CHECK_FALSE(f.fixtures.traps().armed(trap));

    const Breakables& barrels = f.fixtures.barrels();
    std::optional<usize> plain;
    for (usize i = 0; i < barrels.size() && !plain; ++i) {
        if (barrels.standing(i) && barrels.barrel(i).kind == BreakableStrike::Kind::Plain) {
            plain = i;
        }
    }
    REQUIRE(plain.has_value());
    const s32 before = barrels.barrel(*plain).health;
    f.players[0].actor.place(barrels.barrel(*plain).figure.position() + Vec3{0, 0, 1});
    for (s32 frame = 0; frame < 60; ++frame) {
        f.attacks.updateShields(1.0f / 30, f.players, f.targets);
    }
    CHECK(barrels.standing(*plain));
    CHECK(barrels.barrel(*plain).health == before);
}

TEST_CASE("magic under level 25 carries no perk",
          "[game][screens][player-attacks][magic-perks][assets]") {
    const auto root = perkRoot();
    PerkLevel f(root, "G1", "VAL", 24);
    const usize trap = firstShownTrap(f);
    f.castBeside(f.fixtures.traps().trap(trap).figure.position());
    CHECK(f.helps.empty());
    CHECK_FALSE(f.fixtures.traps().trap(trap).disarmed);
}

TEST_CASE("a wave of potion magic reaches a shut chest and makes an apple of Death in it",
          "[game][screens][player-attacks][death-chest][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const auto& infos = f.world.layout().itemInfos();
    const Chests& chests = f.fixtures.chests();
    std::optional<usize> death;
    for (usize i = 0; i < chests.size() && !death; ++i) {
        const s32 inside = chests.chest(i).contents;
        if (inside >= 0 && infos[static_cast<usize>(inside)].name == "DEATH") {
            death = i;
        }
    }
    REQUIRE(death.has_value());
    f.attacks.shatterPotion(1, chests.chest(*death).figure.position() + Vec3{0, 0, 2});
    for (s32 frame = 0; frame < 60; ++frame) {
        f.attacks.updateProjectiles(1.0f / 30, f.players, f.targets);
    }
    CHECK(infos[static_cast<usize>(chests.chest(*death).contents)].name == "APPLE");
    CHECK(chests.chest(*death).state == Chests::kShut); // magic opens nothing
    f.attacks.clear();
    f.fixtures.clear();
}

TEST_CASE("ownerless potions retain their element and cycle only unspecified colors",
          "[game][screens][player-attacks][shattered-potion][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("audio/COMMON.vbk");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.audio.open(root, &sounds, f.world.audio());
    for (const s32 kind : {0, 4, 0, 0, 0, 0}) {
        f.attacks.shatterPotion(kind, Vec3{0});
    }
    constexpr std::array kNames{"MP_FIRE", "MP_ACID", "MP_ELEC", "MP_LIGHT", "MP_ACID", "MP_FIRE"};
    REQUIRE(f.effects.count() == kNames.size());
    for (usize i = 0; i < kNames.size(); ++i) {
        CHECK(f.effects.effect(i).name == kNames[i]);
        CHECK(f.effects.effect(i).scale == Approx(0.5f));
    }
    CHECK(sounds.voiceCount() == 0);
    f.arsenal.burstPotion(1, Vec3{0}, 16); // A player's ordinary cast still sounds.
    CHECK(sounds.voiceCount() == 1);
    f.attacks.clear();
}

TEST_CASE("carried shields and shattered bottles share the party's unspecified potion cycle",
          "[game][screens][player-attacks][cheats][multiplayer][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    Fixture f;
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    auto& actor = f.players[0].actor;
    auto& inventory = actor.save().progress().inventory;
    f.attacks.shieldPotion(0, f.players);
    CHECK(f.attacks.shieldCount() == 0);
    inventory.addPotions(0, 1);
    f.attacks.shieldPotion(0, f.players);
    CHECK(f.attacks.shieldCount() == 1);
    CHECK(inventory.potions.empty());
    const usize before = f.effects.count();
    f.attacks.shatterPotion(0, Vec3{0});
    REQUIRE(f.effects.count() == before + 1);
    CHECK(f.effects.effect(before).name == "MP_ELEC");
    inventory.addPotions(0, 1);
    const auto cast = f.arsenal.usePotion(actor);
    REQUIRE(cast);
    CHECK(cast->potion == 3);
    inventory.addPotions(0, 1);
    f.arsenal.throwPotion(actor);
    REQUIRE(f.arsenal.missiles().count() == 1);
    CHECK(f.arsenal.missiles().missile(0).potion == 4);
    f.attacks.shatterPotion(0, Vec3{0});
    CHECK(f.effects.effect(f.effects.count() - 1).name == "MP_FIRE");
    f.attacks.clear();
}

} // namespace
