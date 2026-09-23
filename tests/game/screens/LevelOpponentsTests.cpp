#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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
        .hurt = [](std::size_t, float, HurtKind, bool) { FAIL("No combatants"); },
        .blast = [](const Vec3&, float, float) { FAIL("No combatants"); },
        .settleBlasts = [&] { phases.emplace_back("blast"); },
        .legend = [](const LegendEvent&) { FAIL("No boss"); },
        .advanceLegend =
            [&](float seconds) {
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("legend");
            },
        .fallen = [](const Vec3&) { FAIL("No boss"); },
        .spew = [](const CritterSpew&) { FAIL("No boss"); },
        .advanceVictory =
            [&](int ticks, float seconds) {
                REQUIRE(ticks == 6);
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("victory");
            },
        .levels = [&] { phases.emplace_back("levels"); },
        .award = [](int, int, bool) { FAIL("No kills"); }};
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
    std::vector<std::size_t> hurt;
    std::vector<HurtKind> kinds;
    LevelOpponents::Events events;
    events.hurt = [&](std::size_t index, float damage, HurtKind kind, bool directed) {
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
    REQUIRE(hurt == std::vector<std::size_t>{0});
    REQUIRE(kinds.back() == HurtKind::Burn);
    REQUIRE(players[0].breathGap == 0.25f);
    fire.critter = 7;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 1);
    fire.player = 1;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt == std::vector<std::size_t>{0, 1});
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
    events.advanceLegend = [](float) {};
    events.advanceVictory = [](int, float) {};
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

TEST_CASE("the level keeps the dragon FIRE effect attached to its animated node",
          "[game][screens][level-opponents][breath][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 25}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().spawn(34, Vec3{0}, 0));
    LevelOpponents::Events events;
    events.hurt = [](std::size_t, float, HurtKind, bool) {};
    events.blast = [](const Vec3&, float, float) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](float) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CritterSpew&) {};
    events.advanceVictory = [](int, float) {};
    events.levels = [] {};
    events.award = [](int, int, bool) {};
    unsigned int fireId = 0;
    int attachedFrames = 0;
    for (int tick = 0; tick < 2400 && attachedFrames < 20; ++tick) {
        opponents.update(1, 1.0f / 60, players, {}, events);
        for (std::size_t e = 0; e < effects.count(); ++e) {
            const auto& effect = effects.effect(e);
            if (effect.name != "FIRE") {
                continue;
            }
            fireId = effect.id;
            REQUIRE(effect.attachment.has_value());
            const auto parent = opponents.bosses().nodeTransform("NODE#01");
            REQUIRE(parent.has_value());
            REQUIRE(effect.transform() == *parent);
            REQUIRE(effect.particles.field().size() == 2);
            ++attachedFrames;
        }
        effects.update(1.0f / 60);
    }
    REQUIRE(attachedFrames == 20);
    REQUIRE(effects.playing(fireId));
    opponents.close();
    REQUIRE_FALSE(effects.playing(fireId));
}
} // namespace
