#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"

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
    Fixture f;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    const LevelAudioInfo info{.bank = "TOWN", .stream = {}};
    f.audio.open(root, &sounds, &info);
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.opponents.open({f.device, f.world, f.weapons, f.effects, f.audio, root, 1}, f.players);
    auto& enemies = f.opponents.enemies();
    // This fixture has no level mesh: retain the routing facade, but spawn
    // its test actors without a floor constraint.
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
} // namespace
