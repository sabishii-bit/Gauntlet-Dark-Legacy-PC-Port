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

TEST_CASE("Lich fissure snapshots the body's facing when detached", "[lich][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
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

TEST_CASE("Lich chain spin reaches nearby players around its body", "[lich][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
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

TEST_CASE("Lich charging spin damages players in its path", "[lich][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
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
          "[lich][level-opponents][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    test::assetOrSkip("MONSTERS/MAG/ANIM.PS2");
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
    s32 livingBrood = 0;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        const auto& enemies = opponents.enemies();
        if (!enemies.alive(id) || enemies.generatorOf(id) < static_cast<s32>(before)) {
            continue;
        }
        ++livingBrood;
        CHECK(enemies.kindOf(id) == 12);
        const s32 way = enemies.algorithmOf(id);
        CHECK((way == kProwlWay || way == kMirroredProwlWay));
    }
    CHECK(livingBrood > 0);
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

TEST_CASE("Lich chain spin delivers damage in the crypt encounter", "[lich][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
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
          "[lich][lich-hands][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG5/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
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
TEST_CASE("Lich stomp and spin keep authored damage reach and independent visual scale",
          "[lich][lich-range][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    const bool stomp = GENERATE(false, true);
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'G');
    REQUIRE(fixture.spawn("LICH", {}, 0));
    EnemyHit phase;
    phase.damage = fixture.actor.maxHealth() * 0.4f;
    fixture.actor.hurt(phase);
    const std::string wanted = stomp ? "STOMP" : "CHAIN";
    const std::string effectName = stomp ? "ATK09FX" : "ATK10FX";
    const std::string node = stomp ? "LEFTHEEL" : "UPPERTORSO";
    const Vec3 offset = stomp ? Vec3{0} : Vec3{0, -5, 0};
    const f32 radius = stomp ? 12.0f : 15.0f;
    const auto* damage = fixture.actor.data()->damage(stomp ? 5 : 6);
    REQUIRE(damage != nullptr);
    CHECK(damage->maxDistance == radius);
    CHECK(damage->minDot == -1);
    std::array<EnemyView, 4> players;
    for (s32 i = 0; i < 4; ++i) {
        players[static_cast<usize>(i)].player = i;
        players[static_cast<usize>(i)].height = 6;
        players[static_cast<usize>(i)].radius = 0.25f;
    }
    bool began = false;
    bool visual = false;
    std::set<s32> hit;
    for (s32 frame = 0; frame < 18000; ++frame) {
        const Vec3 forward{std::sin(fixture.actor.yaw()), 0, std::cos(fixture.actor.yaw())};
        players[0].position = fixture.actor.position() + forward * 10.0f;
        const auto attachment = fixture.actor.nodeTransform(node);
        REQUIRE(attachment);
        const Vec3 origin{*attachment * Vec4{offset, 1}};
        for (usize i = 1; i < players.size(); ++i) {
            const std::array distances{radius * 0.5f, radius - 0.75f, radius + 2.0f};
            players[i].position = origin + Vec3{distances[i - 1], -3, 0};
            players[i].hidden = !began;
        }
        fixture.update(ticks, seconds, players);
        if (fixture.actor.moveName() == wanted) {
            began = true;
        } else if (began) {
            break;
        }
        for (const auto& cue : fixture.actor.takeCues()) {
            if (!began || cue.tree != effectName) {
                continue;
            }
            visual = true;
            CHECK(cue.scale == 1);
            CHECK(cue.follows);
            REQUIRE(cue.placement);
            const auto rootTransform = fixture.actor.rootTransform();
            REQUIRE(rootTransform);
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 4; ++row) {
                    CHECK((*cue.placement)[column][row] == Approx((*rootTransform)[column][row]));
                }
            }
        }
        for (const auto& blow : fixture.actor.takeBlows()) {
            if (began && blow.area && blow.damage > 0) {
                hit.insert(blow.player);
            }
        }
        fixture.actor.takeShots();
    }
    REQUIRE(began);
    REQUIRE(visual);
    CHECK(hit.contains(1));
    CHECK(hit.contains(2));
    CHECK_FALSE(hit.contains(3));
}

TEST_CASE("Lich maggots pursue nearby players but resume prowling outside eight units",
          "[lich][enemies][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/MAG/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(12));
    EnemySpawn spawn;
    spawn.kind = 12;
    spawn.algorithm = 0;
    spawn.generator = 0;
    spawn.placed = true;
    spawn.direction = {0, 0, 1};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    std::array<EnemyView, 1> players{{{0, {20, 0, 0}, 1, 6}}};
    const auto walk = [&](const Vec3& relative) {
        const Vec3 from = enemies.positionOf(*id);
        for (s32 frame = 0; frame < 60; ++frame) {
            players[0].position = enemies.positionOf(*id) + relative;
            enemies.update(2, 1.0f / 30, players);
        }
        return enemies.positionOf(*id) - from;
    };
    // Small species select way 2/4 even when the generator requests way zero.
    const auto way = enemies.algorithmOf(*id);
    REQUIRE((way == kProwlWay || way == kMirroredProwlWay));
    const Vec3 wandering = walk({20, 0, 0});
    CHECK(wandering.z > 0);
    CHECK(wandering.x == Approx(0).margin(0.01f));
    const Vec3 pursuing = walk({7, 0, 0});
    CHECK(pursuing.x > 0);
    CHECK(pursuing.x > std::abs(pursuing.z));
    // Let the eight-update target cadence observe the retreat before changing
    // bearing. Until then its cached close distance still permits pursuit.
    walk({20, 0, 0});
    // Proximity does not permanently convert the maggot into a long-range chaser.
    const Vec3 resumed = walk({0, 0, 20});
    CHECK(resumed.x > 0);
    CHECK(resumed.z == Approx(0).margin(0.01f));
    CHECK(enemies.algorithmOf(*id) == way);
}
} // namespace
