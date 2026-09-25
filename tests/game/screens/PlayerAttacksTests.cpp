#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayerAttacks.h"
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
} // namespace
