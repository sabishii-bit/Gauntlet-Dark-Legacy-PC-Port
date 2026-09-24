#include <array>
#include <set>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/CombatantFixture.h"
#include "game/screens/LevelOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
constexpr Vec3 kHome{-30, 57, -112};

TEST_CASE("Spider Queen exposes her attack families within her authored territory",
          "[spider][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRIDER.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRIDER/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture f;
    f.open(device, root, nullptr, {}, 'D');
    REQUIRE(f.spawn("DRIDER", kHome, 0));
    std::set<std::string> attacks;
    std::set<s32> shots;
    bool pursued = false;
    for (const f32 health : {1.0f, 0.65f, 0.2f}) {
        for (const Vec3 offset :
             {Vec3{0, 0, 10}, Vec3{0, 0, 25}, Vec3{0, 0, 45}, Vec3{20, 0, 10}, Vec3{-20, 0, 10},
              Vec3{0, 0, -10}, Vec3{10, 0, 0}, Vec3{-10, 0, 0}}) {
            REQUIRE(f.actor.spawn(f.assets, 0, kHome, 0, nullptr, {}, 'D'));
            if (health < 1) {
                EnemyHit hit;
                hit.damage = f.actor.maxHealth() * (1 - health);
                f.actor.hurt(hit);
            }
            const std::array<EnemyView, 1> players{{{0, kHome + offset, 1, 6}}};
            for (s32 frame = 0; frame < 3600; ++frame) {
                f.update(2, 1.0f / 30, players);
                if (f.actor.moveType() >= 128) {
                    attacks.emplace(f.actor.moveName());
                }
                for (const auto& shot : f.actor.takeShots()) {
                    shots.insert(shot.damageIndex);
                }
                f.actor.takeCues();
                f.actor.takeBlows();
                const Vec3 delta = f.actor.position() - kHome;
                REQUIRE(glm::length(Vec2{delta.x, delta.z}) <= 22.001f);
                pursued |= glm::length(delta) > 5;
            }
        }
    }
    REQUIRE(pursued);
    REQUIRE(shots == std::set<s32>{6, 7, 8, 9, 10});
    REQUIRE(attacks.contains("WHIPME"));
    REQUIRE(attacks.contains("WHIPRIGHT"));
    REQUIRE(attacks.contains("WHIPLEFT"));
    REQUIRE(attacks.contains("ATTACK4"));
    REQUIRE(attacks.contains("WEBBALL"));
    REQUIRE(attacks.contains("EGGSAC"));
    REQUIRE(attacks.contains("SPITBALL"));
    REQUIRE(attacks.contains("SPIDER"));
    REQUIRE(attacks.contains("ATTACK7"));
    REQUIRE(attacks.contains("ATTACK8"));
    REQUIRE(attacks.contains("ATTACK9"));
    REQUIRE(attacks.contains("KICKBACK"));
    REQUIRE(attacks.contains("CHARGE"));
}

TEST_CASE("Spider Queen egg attacks create generators that breed the stage's spiders",
          "[spider][level-opponents][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELD5/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRIDER/animations.json");
    test::unpackedOrSkip("MONSTERS/SPI/animations.json");
    test::unpackedOrSkip("ITEMS/LEVELD5/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("D5");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {-1, 56.5f, -79}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    const usize before = opponents.generators().count();
    EnemyHit phase;
    phase.damage = opponents.bosses().view().maxHealth * 0.2f;
    opponents.bosses().hurt(phase);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    s32 defeats = 0;
    events.fallen = [&](const Vec3&) { ++defeats; };
    s32 spews = 0;
    events.spew = [&](const CombatSpew&) { ++spews; };
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    bool bred = false;
    std::set<std::string> moves;
    std::set<std::string> visuals;
    for (s32 frame = 0; frame < 3600 && !bred; ++frame) {
        players[0].actor.place(kHome + Vec3{0, 0, 45});
        opponents.update(2, 1.0f / 30, players, {}, events);
        moves.emplace(opponents.bosses().moveName());
        for (usize i = 0; i < effects.count(); ++i) {
            visuals.emplace(effects.effect(i).name);
        }
        effects.update(1.0f / 30);
        for (usize i = before; i < opponents.generators().count(); ++i) {
            REQUIRE(opponents.generators().kindOf(static_cast<s32>(i)) == 9);
            bred |= opponents.generators().bredOf(static_cast<s32>(i)) > 0;
        }
    }
    INFO("Moves: " << Catch::StringMaker<decltype(moves)>::convert(moves));
    INFO("Effects: " << Catch::StringMaker<decltype(visuals)>::convert(visuals));
    REQUIRE(opponents.generators().count() > before);
    INFO("Generator position: " << opponents.generators().positionOf(0).x << ", "
                                << opponents.generators().positionOf(0).y << ", "
                                << opponents.generators().positionOf(0).z);
    REQUIRE(bred);
    EnemyHit hit;
    hit.damage = opponents.bosses().view().maxHealth * 2;
    hit.player = 0;
    opponents.bosses().hurt(hit);
    REQUIRE(defeats == 0);
    for (s32 frame = 0; frame < 900; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
    }
    REQUIRE(defeats == 1);
    REQUIRE(spews == 1);
    REQUIRE_FALSE(opponents.bosses().present());
    opponents.close();
}

TEST_CASE("Bellows poison and shrink the Spider Queen on release for the encounter",
          "[spider][legend][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRIDER.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRIDER/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'D');
    REQUIRE(bosses.spawn(37, kHome, 0));
    REQUIRE(bosses.bringLegend(0));
    const std::array<EnemyView, 1> players{{{0, kHome + Vec3{0, 0, 25}, 1, 6}}};
    const f32 fullHealth = bosses.view().health;
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE(bosses.legend().thrown());
    REQUIRE(bosses.view().health == fullHealth);
    REQUIRE_FALSE(bosses.curbed());
    bosses.landLegend();
    REQUIRE(bosses.view().health == Approx(fullHealth * 0.9f + 2));
    REQUIRE(bosses.curbed());
    REQUIRE(glm::length(Vec3{(*bosses.rootTransform())[0]}) == Approx(0.8f));
    bosses.landLegend();
    REQUIRE(bosses.view().health == Approx(fullHealth * 0.9f + 2));
    bosses.draw(device, Mat4{1}, WorldLighting{});
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            REQUIRE(vertex.color.r <= 64);
            REQUIRE(vertex.color.b <= 64);
        }
    }
    for (s32 frame = 0; frame < 1200; ++frame) {
        bosses.update(2, 1.0f / 30, players);
    }
    REQUIRE_FALSE(bosses.legend().running());
    REQUIRE_FALSE(bosses.legend().darkens());
    REQUIRE(bosses.curbed());
}
} // namespace
