#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/Damage.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/players/MagicPerks.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PlayerAttacks.h"
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
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 1> players;
    PlayerAttacks::Targets targets{opponents, fixtures, {}};
    Fixture() {
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    }
};

std::filesystem::path turboAssets() {
    const auto root = test::scratchDirectory("turbo-contacts");
    for (const auto* name : {"PLAYERS/WAR/YEL", "MONSTERS/GRU"}) {
        const auto dir = root / name;
        std::filesystem::create_directories(dir);
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
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
    std::filesystem::create_directories(root / "pdata");
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0,"turboC1":1},"moveStrikes":[
      {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50},
      {"type":2,"startFrame":1,"hitRadius":10,"arc":-1,"offset":[0,9,2],
       "speedMin":30,"speedMax":30,"maxTime":6,"amount":70,"flags":64,
       "damageType":1048576}]})");
    return root;
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

TEST_CASE("every exported class can damage enemies with both turbo attacks",
          "[game][screens][player-attacks][turbo-roster][unpacked]") {
    const auto root = test::unpackedOrSkip("pdata/JES.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GRU/animations.json");
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

TEST_CASE("Jester turbo damage reaches bosses great creatures and generators",
          "[game][screens][player-attacks][turbo-roster][unpacked]") {
    const auto root = test::unpackedOrSkip("pdata/JES.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::unpackedOrSkip("MONSTERS/GOLEM/LEVELG/animations.json");
    test::unpackedOrSkip("MONSTERS/GRU/animations.json");
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
          "[game][items][unpacked]") {
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
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
          "[game][screens][player-attacks][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/ZOM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
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
          "[game][screens][player-attacks][projectile-impact][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
    test::unpackedOrSkip("audio/COMMON/sounds.json");
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
          "[game][screens][player-attacks][walls][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("audio/COMMON/sounds.json");
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

TEST_CASE("a blow on a secret wall tells of multiple hits; a swing passes the safe rocks by",
          "[game][screens][player-attacks][walls][melee][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("LEVELS/LEVELB6/world.json");
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
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
    f.fixtures.clear();
}

TEST_CASE("a thrown weapon stops at a chest and does it no harm",
          "[game][screens][player-attacks][item-stops][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
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
          "[game][screens][player-attacks][triggers][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELC3/world.json").parent_path().parent_path().parent_path();
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
    CHECK(shot);
    f.fixtures.clear();
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
        // A tenth of the harm, and 0.016 more a level past 75.
        const auto expected = static_cast<s32>(std::lround(harm * (0.1f + 0.016f * 5.0f)));
        CHECK(save.health() == (heals ? 100 + expected : 100));
        CHECK((std::ranges::find(helps, HelpMessages::kHealingMagic) != helps.end()) == heals);
        CHECK(std::ranges::find(helps, HelpMessages::kWastedMagic) == helps.end());
        f.attacks.clear();
        f.opponents.close();
    }
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
    const auto expected = static_cast<s32>(std::lround(harm * (0.1f + 0.016f * 5.0f)));
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
          "[game][screens][player-attacks][melee][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/GRU/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
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

TEST_CASE("a halo wearer with Death the nearest thing ahead holds him and draws him off",
          "[game][screens][player-attacks][death][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
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
    const auto death =
        enemies.spawn({.kind = kDeathKind, .tier = 1, .position = {0, 0, 10}, .placed = true}, {});
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
    f.opponents.close();
}

TEST_CASE("fire and lightning shields harm the creature their bearer stands against",
          "[game][screens][player-attacks][shield][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/GRU/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
    test::unpackedOrSkip("WEAPONS/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
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

TEST_CASE("the melee sees what is near at any bearing, within a swing or a step",
          "[game][screens][player-attacks][melee][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/GRU/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
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
    // Behind the character: still within a swing, which turns to it.
    actor.place({0, 0, 5});
    actor.turnTo(0.0f);
    const MeleeSense behind = f.attacks.meleeSense(actor, true, f.targets);
    CHECK(behind.range == MeleeRange::Swing);
    CHECK(std::abs(behind.yaw) == Approx(std::numbers::pi_v<f32>).margin(0.01f));
    const PlayerDeed close = f.attacks.attackDeed(actor, false, f.targets);
    CHECK((close == PlayerDeed::Melee || close == PlayerDeed::MeleeLow));
    // Backing off: a step away, it is stepped to only while the stick moves.
    bool stepped = false;
    for (f32 z = 5.0f; z < 12.0f && !stepped; z += 0.1f) {
        actor.place({0, 0, z});
        const MeleeSense sense = f.attacks.meleeSense(actor, true, f.targets);
        if (sense.range != MeleeRange::Step) {
            continue;
        }
        stepped = true;
        CHECK(f.attacks.attackDeed(actor, false, f.targets) == PlayerDeed::Attack);
        CHECK(f.attacks.attackDeed(actor, false, f.targets, true) ==
              (sense.low ? PlayerDeed::Attack : PlayerDeed::Melee));
        CHECK(f.attacks.attackDeed(actor, true, f.targets, true) ==
              (sense.low ? PlayerDeed::StrongAttack : PlayerDeed::MeleeSlow));
        // Let go of, the attack reaches a unit less.
        CHECK(f.attacks.meleeSense(actor, false, f.targets).range != MeleeRange::Swing);
    }
    CHECK(stepped);
    actor.place({0, 0, 20});
    CHECK(f.attacks.meleeSense(actor, true, f.targets).range == MeleeRange::Beyond);
    enemies.close();
}

TEST_CASE("a melee contact routes damage sound and impact once through level opponents",
          "[game][screens][player-attacks][melee][enemy-feedback][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/ZOM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("PLAYERS/WAR/ANIM/animations.json");
    test::unpackedOrSkip("WEAPONS/animations.json");
    test::unpackedOrSkip("audio/TOWN/sounds.json");
    test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
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
          "[game][items][player-attacks][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/ZOM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
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
          "[game][items][player-attacks][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/ZOM/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
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
          "[game][screens][player-attacks][chimera][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/CHIMERA.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
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
          "[game][screens][player-attacks][shattered-potion][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
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
    f.targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&) {};
    f.targets.fixtureEvents.help = [](s32, usize) {
        FAIL("A bottle is not destroyed food");
        return false;
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

TEST_CASE("a thrown weapon breaks a bottle lying about: its magic and the thrower's both go off",
          "[game][screens][player-attacks][shot-potion][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
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
    REQUIRE(f.arsenal.missiles().launch(launch));
    f.attacks.updateProjectiles(0.5f, f.players, f.targets);
    CHECK(f.world.placedItems().item(bottle).taken);
    // The bottle's own blue magic and the thrower's, the thrower told shooting does less.
    usize bursts = 0;
    for (usize i = 0; i < f.effects.count(); ++i) {
        bursts += f.effects.effect(i).name == "MP_ELEC" ? 1 : 0;
    }
    CHECK(bursts == 2);
    CHECK(helps == std::vector<s32>{HelpMessages::kShotMagic});
    // Magic, hand blows and aiming never take a bottle for a target.
    CHECK(f.world.placedItems().shootablePotions().empty());
    f.attacks.clear();
    f.fixtures.clear();
}

TEST_CASE("potion magic leaves the plain, exploding and gas barrels alone but breaks one that "
          "holds something",
          "[game][screens][player-attacks][magic-immunity][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.targets.fixtureEvents.help = [](s32, usize) { return true; };
    f.targets.fixtureEvents.hurt = [](usize, f32, HurtKind, bool) {};
    f.targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&) {};
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
        targets.fixtureEvents.hurt = [](usize, f32, HurtKind, bool) {};
        targets.fixtureEvents.opponents = [](const Vec3&, f32, f32, std::vector<s32>&) {};
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
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
    return root;
}

TEST_CASE("the warriors' magic turns the junk in a gold chest into silver, and from 50 gold",
          "[game][screens][player-attacks][magic-perks][unpacked]") {
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
          "[game][screens][player-attacks][magic-perks][unpacked]") {
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
          "[game][screens][player-attacks][magic-perks][unpacked]") {
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
          "[game][screens][player-attacks][magic-perks][walls][unpacked]") {
    const auto root = perkRoot();
    test::unpackedOrSkip("LEVELS/LEVELE1/world.json");
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
          "[game][screens][player-attacks][magic-perks][unpacked]") {
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
          "[game][screens][player-attacks][magic-perks][unpacked]") {
    const auto root = perkRoot();
    PerkLevel f(root, "G1", "VAL", 24);
    const usize trap = firstShownTrap(f);
    f.castBeside(f.fixtures.traps().trap(trap).figure.position());
    CHECK(f.helps.empty());
    CHECK_FALSE(f.fixtures.traps().trap(trap).disarmed);
}

TEST_CASE("a wave of potion magic reaches a shut chest and makes an apple of Death in it",
          "[game][screens][player-attacks][death-chest][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
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
          "[game][screens][player-attacks][shattered-potion][unpacked]") {
    const auto root = test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    test::unpackedOrSkip("audio/COMMON/sounds.json");
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

} // namespace
