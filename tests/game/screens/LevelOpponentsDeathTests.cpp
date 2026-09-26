#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerHealth.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("Death drains bypass armor without bypassing invulnerability or normal death handling",
          "[death][player-health]") {
    const bool invulnerable = GENERATE(false, true);
    PlayerRuntime player;
    player.actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    auto& progress = player.actor.save().progress();
    progress.health = 2;
    if (invulnerable) {
        progress.inventory.addPowerup(powerup::kArmor, 0x10000, 30, 30);
    }
    PlayerHealth health;
    PlayerHealth::Events events;
    s32 deaths = 0;
    events.sound = [&](std::string_view sound) {
        CHECK(sound == "S_PLAYERDIES");
        ++deaths;
    };
    events.cry = [](std::string_view cry) { CHECK(cry == "DIE2"); };
    events.named = [](std::string_view) {};
    ClassStats stats;
    stats.armorMin = 999;
    stats.armorMax = 999;
    for (s32 i = 0; i < 2; ++i) {
        health.hurt(player, 1, HurtKind::DeathDrain, true, false, 20, events,
                    PlayerImpact{.flags = 0x1000}, false, &stats);
    }
    CHECK(player.life == (invulnerable ? PlayerLife::Standing : PlayerLife::Dying));
    CHECK(deaths == (invulnerable ? 0 : 1));
    CHECK(player.hitFlashTicks == 0);
}
TEST_CASE("Anti Death is exposed by player identity only while the item is active", "[death]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                            DeathRules::kProtection, 30, 30);
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 1);
    CHECK(views[0].player == 3);
    CHECK(views[0].antiDeath);
}

void verifyDeathTransfers(s32 tier, bool realAssets) {
    const auto root = realAssets ? test::unpackedOrSkip("MONSTERS/DEATH/animations.json")
                                       .parent_path()
                                       .parent_path()
                                       .parent_path()
                                 : test::deathArchive();
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AudioMixer mixer{48000};
    SoundPlayer sound(mixer);
    if (realAssets) {
        audio.open(root, &sound, nullptr);
    }
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(1, {}, nullptr, {100, 0, 100}, 0);
    players[1].actor.spawn(3, {}, nullptr, {0, 0, 2}, 0);
    auto& progress = players[1].actor.save().progress();
    progress.experience = levelExperience(30) + 200;
    progress.health = 2000;
    const s32 experience = progress.experience;
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    opponents.enemies().open(device, root, nullptr, 2, {}, 1);
    REQUIRE(opponents.enemies().loadKind(kDeathKind));
    const auto id =
        opponents.enemies().spawn({.kind = kDeathKind, .tier = tier, .placed = true}, {});
    REQUIRE(id);
    PlayerHealth health;
    PlayerHealth::Events healthEvents;
    healthEvents.sound = [](std::string_view) {};
    healthEvents.cry = [](std::string_view) {};
    healthEvents.named = [](std::string_view) {};
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.hurt = [&](usize player, f32 amount, HurtKind kind, bool directed,
                      const PlayerImpact& impact) {
        CHECK(player == 1);
        CHECK(kind == HurtKind::DeathDrain);
        health.hurt(players[player], amount, kind, directed, false, 1, healthEvents, impact);
    };
    for (s32 i = 0; i < 90 && progress.health == 2000 && progress.experience == experience; ++i) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    CHECK(progress.health == (tier == 1 ? 1999 : 2000));
    CHECK(progress.experience == experience - (tier == 2 ? 27 : 0));
    REQUIRE(effects.count() == 1);
    CHECK(effects.effect(0).name == DeathRules::effect(DeathRules::form(tier)));
    const u32 effect = effects.effect(0).id;
    effects.update(1);
    CHECK(effects.playing(effect));
    effects.draw(device, Mat4{1}, {});
    CHECK_FALSE(device.draws.empty());
    if (realAssets) {
        CHECK(sound.voiceCount() > 0);
        std::array<f32, 2048> samples{};
        mixer.mix(samples);
        CHECK(std::ranges::any_of(samples, [](f32 sample) { return sample != 0; }));
    }
    players[1].actor.place({0, 0, 100});
    opponents.update(2, 1.0f / 30, players, {}, events);
    CHECK_FALSE(effects.playing(effect));
    if (realAssets) {
        std::vector<f32> silence(usize{48000} * 2 * 8);
        mixer.mix(silence);
        sound.update();
        CHECK(sound.voiceCount() == 0);
    }
    players[1].actor.place(opponents.enemies().positionOf(*id) + Vec3{0, 0, 2});
    for (s32 i = 0; i < 3; ++i) {
        opponents.update(2, 1.0f / 30, players, {}, events);
    }
    REQUIRE(effects.count() == 1);
    CHECK(effects.effect(0).id != effect);
    opponents.strikeEnemy(*id, 1, EnemyHit::kMagic, Vec3{0}, 3, players);
    opponents.update(2, 1.0f / 30, players, {}, events);
    CHECK(effects.count() == 0);
    CHECK(opponents.enemies().dying(*id));
    opponents.close();
    audio.close();
    std::array<f32, 2048> tail{};
    mixer.mix(tail);
    sound.update();
    CHECK(sound.voiceCount() == 0);
}
TEST_CASE("Death transfers affect only its contact player and stop effects on separation",
          "[death][level-opponents]") {
    verifyDeathTransfers(GENERATE(1, 2), false);
}
TEST_CASE("Death plays its retail drain artwork and releases its looping audio",
          "[death][level-opponents][unpacked]") {
    verifyDeathTransfers(GENERATE(1, 2), true);
}

TEST_CASE("Authored Red and Black Death placements enter the level roster",
          "[death][level-opponents][unpacked]") {
    const bool black = GENERATE(false, true);
    const auto root = test::unpackedOrSkip("MONSTERS/DEATH/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip(black ? "LEVELS/LEVELA1/world.json" : "LEVELS/LEVELE1/world.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(black ? "A1" : "E1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::vector<PlayerRuntime> players(black ? 2 : 1);
    for (usize i = 0; i < players.size(); ++i) {
        players[i].actor.spawn(static_cast<s32>(i), {}, nullptr, Vec3{0}, 0);
    }
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    const Vec3 expected = black ? Vec3{82, -6.84375f, 61.96875f} : Vec3{60.75f, 0.15625f, 0};
    s32 found = -1;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        const Vec3 at = opponents.enemies().positionOf(id);
        if (opponents.enemies().alive(id) && opponents.enemies().kindOf(id) == kDeathKind &&
            glm::distance(Vec2{at.x, at.z}, Vec2{expected.x, expected.z}) < 0.1f) {
            found = id;
            break;
        }
    }
    INFO("Level " << level->name << ", roster " << opponents.enemies().count());
    REQUIRE(found >= 0);
    CHECK(opponents.enemies().tierOf(found) == (black ? 2 : 1));
    CHECK(opponents.enemies().healthOf(found) == 100);
    const auto floor = world.collision().floorAt(expected, 2, 2);
    REQUIRE(floor);
    CHECK(opponents.enemies().positionOf(found).y == floor->y);

    const Vec3 playerPosition = black ? Vec3{82, floor->y, 56} : Vec3{55, floor->y, 0};
    for (auto& player : players) {
        player.actor.place(playerPosition);
    }
    s32 drains = 0;
    for (s32 frame = 0; frame < 180; ++frame) {
        opponents.enemies().update(2, 1.0f / 30, LevelOpponents::enemyViews(players));
        for (const auto& cue : opponents.enemies().takeDeathEvents()) {
            drains += cue.enemy == found && cue.kind == DeathEvent::Kind::Drain ? 1 : 0;
        }
    }
    const Vec3 stopped = opponents.enemies().positionOf(found);
    INFO("Final Death position " << stopped.x << ',' << stopped.y << ',' << stopped.z);
    CHECK(drains > 0);

    // Container param[4] chooses Black Death; it is not an ordinary pickup count.
    // GUNE5D fn_8005E90C selects strength 2 when this field is nonzero.
    const auto& infos = world.layout().itemInfos();
    const auto record = std::ranges::find_if(infos, [](const ItemInfo& info) {
        return info.type == ItemInfo::kPlacedEnemy && enemyKindOf(info.name) == kDeathKind;
    });
    REQUIRE(record != infos.end());
    opponents.enemies().open(device, root, nullptr, 2, {}, 1);
    CHECK_FALSE(opponents.releaseDeath(-1, {}, 0));
    const auto index = static_cast<s32>(record - infos.begin());
    REQUIRE(opponents.releaseDeath(index, {}, black ? 1 : 0));
    CHECK(opponents.enemies().count() == 1);
    CHECK(opponents.enemies().tierOf(0) == (black ? 2 : 1));
    opponents.close();
}
} // namespace
