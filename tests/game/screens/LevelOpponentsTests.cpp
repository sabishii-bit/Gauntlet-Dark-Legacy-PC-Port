#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelOpponents.h"
namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("opponent views preserve player identity and hide fallen participants",
          "[game][screens][level-opponents]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{30, 0, 40}, 0);
    players[1].life = PlayerLife::Dying;
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 2);
    REQUIRE(views[0].player == 3);
    REQUIRE(views[0].position == players[0].actor.position());
    REQUIRE(views[0].radius == players[0].actor.radius());
    REQUIRE_FALSE(views[0].hidden);
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
        .hurt = [](usize, f32, HurtKind, bool) { FAIL("No combatants"); },
        .blast = [](const Vec3&, f32, f32) { FAIL("No combatants"); },
        .settleBlasts = [&] { phases.emplace_back("blast"); },
        .legend = [](const LegendEvent&) { FAIL("No boss"); },
        .advanceLegend =
            [&](f32 seconds) {
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("legend");
            },
        .fallen = [](const Vec3&) { FAIL("No boss"); },
        .spew = [](const CritterSpew&) { FAIL("No boss"); },
        .advanceVictory =
            [&](s32 ticks, f32 seconds) {
                REQUIRE(ticks == 6);
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("victory");
            },
        .levels = [&] { phases.emplace_back("levels"); },
        .award = [](s32, s32, bool) { FAIL("No kills"); }};
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
    events.hurt = [&](usize index, f32 damage, HurtKind kind, bool directed) {
        REQUIRE(damage == 40);
        REQUIRE(directed);
        hurt.push_back(index);
        kinds.push_back(kind);
    };
    CritterBlow fire;
    fire.player = 3;
    fire.damage = 40;
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

TEST_CASE("the level keeps boss effects on their animated node or full model root",
          "[game][screens][level-opponents][breath][boss-effects][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    s32 kind = 34;
    f32 distance = 25;
    bool rootEffect = false;
    CritterData lichData;
    SECTION("Dragon FIRE rides its animated mouth") {}
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
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().spawn(kind, Vec3{0}, 0));
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CritterSpew&) {};
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
} // namespace
