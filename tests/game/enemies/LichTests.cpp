#include <array>
#include <cmath>
#include <set>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CombatantFixture.h"
#include "game/screens/LevelOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Lich fissure snapshots the body's facing when detached", "[lich][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'G');
    REQUIRE(fixture.spawn("LICH", {4, 0, 6}, 1.2f));
    std::array<EnemyView, 1> players;
    players[0].player = 0;
    players[0].height = 6;
    players[0].radius = 1;
    bool seen = false;
    for (s32 frame = 0; frame < 9000 && !seen; ++frame) {
        const Vec3 forward{std::sin(fixture.actor.yaw()), 0, std::cos(fixture.actor.yaw())};
        players[0].position = fixture.actor.position() + forward * 20.0f;
        fixture.update(2, 1.0f / 30, players);
        for (const auto& cue : fixture.actor.takeCues()) {
            if (cue.tree == "A13") {
                seen = true;
                CHECK_FALSE(cue.follows);
                CHECK(cue.yaw == Approx(fixture.actor.yaw()));
            }
        }
        fixture.actor.takeBlows();
        fixture.actor.takeShots();
    }
    REQUIRE(seen);
}

TEST_CASE("Lich chain spin reaches nearby players around its body", "[lich][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'G');
    REQUIRE(fixture.spawn("LICH", {}, 0));
    std::array<EnemyView, 4> players;
    for (s32 i = 0; i < 4; ++i) {
        players[static_cast<usize>(i)].player = i;
        players[static_cast<usize>(i)].height = 6;
        players[static_cast<usize>(i)].radius = 1;
    }
    const std::array offsets{Vec3{0, 0, 10}, Vec3{10, 0, 0}, Vec3{0, 0, -10}, Vec3{-10, 0, 0}};
    std::set<s32> struck;
    bool chain = false;
    for (s32 frame = 0; frame < 9000; ++frame) {
        for (usize i = 0; i < players.size(); ++i) {
            players[i].position = fixture.actor.position() + offsets[i];
            players[i].hidden = i != 0 && !chain;
        }
        fixture.update(2, 1.0f / 30, players);
        if (fixture.actor.moveName() == "CHAIN") {
            chain = true;
        } else if (chain) {
            break;
        }
        for (const auto& blow : fixture.actor.takeBlows()) {
            if (chain && blow.area) {
                struck.insert(blow.player);
            }
        }
        fixture.actor.takeCues();
        fixture.actor.takeShots();
    }
    REQUIRE(chain);
    CHECK(struck == std::set<s32>{0, 1, 2, 3});
}

TEST_CASE("Lich charging spin damages players in its path", "[lich][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'G');
    REQUIRE(fixture.spawn("LICH", {}, 0));
    std::array<EnemyView, 1> players{{{0, {0, 0, 40}, 1, 6}}};
    bool charging = false;
    bool hit = false;
    for (s32 frame = 0; frame < 9000; ++frame) {
        if (charging) {
            players[0].position = fixture.actor.position() + Vec3{0, 0, 8};
        }
        fixture.update(2, 1.0f / 30, players);
        if (fixture.actor.moveName() == "CHARGE") {
            charging = true;
        } else if (charging) {
            break;
        }
        for (const auto& blow : fixture.actor.takeBlows()) {
            hit |= charging && blow.area && blow.damage > 0;
        }
        fixture.actor.takeCues();
        fixture.actor.takeShots();
    }
    REQUIRE(charging);
    CHECK(hit);
}

TEST_CASE("Lich spit creates visible generators that breed the stage's maggots",
          "[lich][level-opponents][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::unpackedOrSkip("MONSTERS/MAG/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G5");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0, 20}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.bosses().wake();
    // SPIT opens at rate .75 (about 94% health), then returns below 53%.
    // Full-health Lich intentionally has no generator-spit move available.
    EnemyHit phase;
    const f32 damageFraction = GENERATE(0.1f, 0.6f);
    phase.damage = opponents.bosses().view().maxHealth * damageFraction;
    opponents.bosses().hurt(phase);
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
    const usize before = opponents.generators().count();
    bool bred = false;
    std::set<std::string> moves;
    std::set<std::string> visuals;
    for (s32 frame = 0; frame < 9000 && !bred; ++frame) {
        players[0].actor.place(*opponents.bosses().position() + Vec3{0, 0, 20});
        opponents.update(2, 1.0f / 30, players, {}, events);
        moves.emplace(opponents.bosses().moveName());
        for (usize i = 0; i < effects.count(); ++i) {
            visuals.emplace(effects.effect(i).name);
        }
        effects.update(1.0f / 30);
        for (usize i = before; i < opponents.generators().count(); ++i) {
            bred |= opponents.generators().bredOf(static_cast<s32>(i)) > 0;
        }
    }
    INFO("Moves: " << Catch::StringMaker<decltype(moves)>::convert(moves));
    INFO("Effects: " << Catch::StringMaker<decltype(visuals)>::convert(visuals));
    CHECK(moves.contains("SPIT"));
    CHECK(opponents.generators().count() > before);
    CHECK(bred);
    for (usize i = before; i < opponents.generators().count(); ++i) {
        const auto id = static_cast<s32>(i);
        CHECK(opponents.generators().kindOf(id) == 12);
        CHECK(opponents.generators().bodyShown(id));
        const auto position = opponents.generators().positionOf(id);
        const auto floor = world.collision().floorAt(position, 0.5f, 3);
        REQUIRE(floor);
        CHECK(position.y == Approx(floor->y + ItemFigure::kFloorLift));
        device.draws.clear();
        opponents.generators().draw(device, Mat4{1}, WorldLighting{});
        CHECK_FALSE(device.draws.empty());
        const auto destroyed = opponents.generators().strike(id, 1000, 0);
        REQUIRE(destroyed);
        CHECK(destroyed->destroyed);
        CHECK_FALSE(opponents.generators().bodyShown(id));
    }
}

TEST_CASE("Lich chain spin delivers damage in the crypt encounter", "[lich][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
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
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0, 12}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.bosses().wake();
    LevelOpponents::Events events;
    s32 hits = 0;
    events.hurt = [&](usize, f32 damage, HurtKind, bool, const PlayerImpact&) {
        if (opponents.bosses().moveName() == "CHAIN" && damage > 0) {
            ++hits;
        }
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
    bool chain = false;
    for (s32 frame = 0; frame < 9000; ++frame) {
        const Vec3 boss = *opponents.bosses().position();
        const Vec3 position = boss + Vec3{0, 0, 12};
        const auto floor = world.collision().floorAt(position, 5, 5);
        REQUIRE(floor);
        players[0].actor.place({position.x, floor->y, position.z});
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
        if (opponents.bosses().moveName() == "CHAIN") {
            chain = true;
        } else if (chain) {
            break;
        }
    }
    REQUIRE(chain);
    CHECK(hits > 0);
}
TEST_CASE("Lich ground hands damage and hinder the player standing in their grasp",
          "[lich][lich-hands][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
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
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0.2f, 45}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.bosses().wake();
    // HANDGRAB becomes eligible at the damaged boss's faster attack rate.
    EnemyHit phase;
    phase.damage = opponents.bosses().view().maxHealth * 0.6f;
    opponents.bosses().hurt(phase);
    LevelOpponents::Events events;
    s32 hits = 0;
    events.hurt = [&](usize, f32 damage, HurtKind, bool, const PlayerImpact& impact) {
        if ((impact.flags & PlayerImpact::kSticky) != 0) {
            CHECK(damage > 0);
            CHECK(impact.reaction(damage, 0, false) == PlayerDeed::Webbed);
            ++hits;
        }
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
    bool hands = false;
    std::set<std::string> moves;
    for (s32 frame = 0; frame < 9000 && hits == 0; ++frame) {
        if (!hands) {
            players[0].actor.place(*opponents.bosses().position() + Vec3{0, 0.2f, 45});
        }
        opponents.update(2, 1.0f / 30, players, {}, events);
        moves.emplace(opponents.bosses().moveName());
        for (usize i = 0; i < effects.count(); ++i) {
            if (effects.effect(i).name == "ATK14GENFX") {
                hands = true;
            }
        }
        effects.update(1.0f / 30);
    }
    INFO("Moves: " << Catch::StringMaker<decltype(moves)>::convert(moves));
    REQUIRE(hands);
    CHECK(hits > 0);
}
} // namespace
