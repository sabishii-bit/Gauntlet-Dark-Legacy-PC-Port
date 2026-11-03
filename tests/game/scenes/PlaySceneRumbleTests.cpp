#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/ControlProfiles.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("scene damage vibrates the actual input player and pause or close stops motors",
          "[game][screens][assets][rumble]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    test::RecordingHaptics haptics;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f};
    CharacterSave save;
    save.progress().health = 2000;
    const std::array party{PartyMember{3, save}, PartyMember{1, save}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options, {.haptics = &haptics}));
    const auto entering = [&] {
        return scene.spawning() || scene.animator(3)->entering() || scene.animator(1)->entering();
    };
    for (s32 i = 0; i < 400 && entering(); ++i) {
        scene.update(1.0 / 60.0, {});
    }
    REQUIRE_FALSE(entering());
    CHECK(std::ranges::all_of(haptics.pulses, [](const auto& pulse) {
        return pulse.feedback == ControlFeedback::Damage;
    }));
    haptics.pulses.clear();
    scene.harm(1, 5, HurtKind::Blow);
    REQUIRE(haptics.pulses.size() == 1);
    CHECK(haptics.pulses.back() == test::RecordingHaptics::Pulse{1, 10, ControlFeedback::Damage});
    scene.harm(3, 5, HurtKind::Blow);
    REQUIRE(haptics.pulses.size() == 2);
    CHECK(haptics.pulses.back() == test::RecordingHaptics::Pulse{3, 10, ControlFeedback::Damage});
    const s32 before = haptics.stops;
    scene.pauseGameplaySounds();
    CHECK(haptics.stops == before + 1);
    scene.close();
    CHECK(haptics.stops == before + 2);
    scene.close(); // a closed scene no longer borrows a potentially destroyed application
    CHECK(haptics.stops == before + 2);
}

TEST_CASE("a real melee animation gives one hit pulse to its attacker's input slot",
          "[game][screens][assets][rumble][melee-rumble]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 fps = GENERATE(30, 60, 120);
    GameConfig config;
    config.combat.autoMelee = true;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    test::RecordingHaptics haptics;
    constexpr s32 kPlayer = 3;
    const test::RecordingHaptics::Pulse hit{kPlayer, 0, ControlFeedback::MeleeHit};
    CharacterSave save;
    save.autoAttack = true;
    save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
    const std::array party{PartyMember{kPlayer, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{18.8f, 0.2f, 0.7f};
    options.yaw = kPi * 0.5f;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options, {.haptics = &haptics}));
    const auto entering = [&] { return scene.spawning() || scene.animator(kPlayer)->entering(); };
    const f64 step = 1.0 / fps;
    for (s32 frame = 0; frame < fps * 7 && entering(); ++frame) {
        scene.update(step, {});
    }
    REQUIRE_FALSE(entering());
    REQUIRE(haptics.pulses.empty());
    const auto& actor = *scene.actor(kPlayer);
    auto& enemies = scene.enemies();
    REQUIRE(enemies.within(actor.position(), 8).empty());
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.position = actor.position() + Vec3{4.5f, 0, 0};
    spawn.direction = {-1, 0, 0};
    const auto enemy = enemies.spawn(spawn, {});
    REQUIRE(enemy);
    const f32 health = enemies.healthOf(*enemy);
    for (s32 frame = 0; frame < fps * 3 && enemies.healthOf(*enemy) == health; ++frame) {
        REQUIRE(haptics.pulses.empty()); // Neither acquiring nor winding up is a hit.
        const Vec3 toward = enemies.positionOf(*enemy) - actor.position();
        const f32 heading = std::atan2(toward.x, toward.z) - scene.viewCamera().yaw;
        PlayScene::Inputs input{};
        input[kPlayer].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
        REQUIRE(scene.update(step, input) == PlayOutcome::Running);
    }
    REQUIRE(enemies.healthOf(*enemy) < health);
    REQUIRE(haptics.pulses == std::vector{hit});
    // Finishing the animation without another attack cannot repeat that contact.
    for (s32 frame = 0; frame < fps; ++frame) {
        scene.update(step, {});
    }
    CHECK(haptics.pulses == std::vector{hit});
}
TEST_CASE("shooting a generator gives its destroying player one pulse rather than one per hit",
          "[game][screens][assets][rumble][generator-rumble]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 fps = GENERATE(30, 60);
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    test::RecordingHaptics haptics;
    constexpr s32 kPlayer = 3;
    const test::RecordingHaptics::Pulse destroyed{kPlayer, 0, ControlFeedback::GeneratorDestroyed};
    CharacterSave save;
    save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
    const std::array party{PartyMember{kPlayer, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{100.0f, 10.2f, -72.5f};
    options.yaw = kPi / 2.0f;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options, {.haptics = &haptics}));
    const f64 step = 1.0 / fps;
    for (s32 frame = 0;
         frame < fps * 7 && (scene.spawning() || scene.animator(kPlayer)->entering()); ++frame) {
        scene.update(step, {});
    }
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.animator(kPlayer)->entering());
    REQUIRE(haptics.pulses.empty());
    s32 generator = -1;
    for (usize i = 0; i < scene.generators().count(); ++i) {
        const auto id = static_cast<s32>(i);
        if (glm::distance(scene.generators().positionOf(id), Vec3{111.25f, 10.13f, -72.5f}) < 1) {
            generator = id;
        }
    }
    REQUIRE(generator >= 0);
    REQUIRE(scene.generators().standing(generator));
    const f32 health = scene.generators().healthOf(generator);
    bool nonlethal = false;
    PlayScene::Inputs attack{};
    attack[kPlayer].attack = true;
    for (s32 frame = 0; frame < fps * 60 && scene.generators().standing(generator); ++frame) {
        scene.update(step, attack);
        if (scene.generators().standing(generator)) {
            nonlethal |= scene.generators().healthOf(generator) < health;
            REQUIRE(haptics.pulses.empty());
        }
    }
    CHECK(nonlethal);
    REQUIRE_FALSE(scene.generators().standing(generator));
    // Projectiles resolve after opponents; their reward/feedback queue settles next tick.
    scene.update(step, {});
    REQUIRE(haptics.pulses == std::vector{destroyed});
    for (s32 frame = 0; frame < fps; ++frame) {
        scene.update(step, {});
    }
    CHECK(haptics.pulses == std::vector{destroyed});
}
} // namespace
