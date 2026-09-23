#include <array>
#include <set>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CombatantFixture.h"
#include "game/enemies/SkorneRelics.h"
#include "game/screens/BossSequence.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Temple Skorne drops four relics in a fixed arc without coin speed scaling", "[skorne]") {
    const auto drops = SkorneRelics::spray({0, 30, 40}, 1);
    REQUIRE(drops[0].name == "BGNTR_IC");
    REQUIRE(drops[1].name == "BMASK_IC");
    REQUIRE(drops[2].name == "BHORN_IC");
    REQUIRE(drops[3].name == "BGNTL_IC");
    for (const auto& drop : drops) {
        REQUIRE(drop.velocity.y == 30);
        REQUIRE(glm::length(drop.velocity) == Catch::Approx(50));
    }
    REQUIRE(drops.front().velocity.x == Catch::Approx(-drops.back().velocity.x));
    REQUIRE(drops.front().velocity.x < drops[1].velocity.x);
    REQUIRE(drops[1].velocity.x < drops[2].velocity.x);
    REQUIRE(SkorneRelics::kStrength == 240);
}

TEST_CASE("Temple Skorne death creates collectable timed relics from the level records",
          "[skorne][boss-sequence][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE2/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE2/animations.json");
    test::unpackedOrSkip("POWERUPS/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E2");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const usize before = world.placedItems().size();
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 2> players;
    BossSequence sequence;
    sequence.bind({device, world, weapons, textures, effects, audio, &catalog});
    CombatSpew spew;
    spew.origin = {0, 0, 0};
    spew.velocity = {0, 30, 40};
    spew.halfAngle = 1;
    sequence.spewCoins(spew, opponents, players);
    REQUIRE(world.placedItems().size() == before + 4);
    constexpr std::array<u32, 4> kFlags{0x4000, 0x2000, 0x1000, 0x8000};
    for (usize i = 0; i < 4; ++i) {
        const auto& item = world.placedItems().item(before + i);
        REQUIRE(item.subtype == 9);
        REQUIRE(item.flags == kFlags[i]);
        REQUIRE(item.strength == 240);
        REQUIRE(item.noGrabSeconds == 2);
        REQUIRE(item.thrown);
        REQUIRE_FALSE(item.takeable());
    }
    sequence.clear();
}

TEST_CASE("Skorne entrance sends three masonry cues across both start animations",
          "[skorne][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELE2/world.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.byName("E2").has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("E2")));
    REQUIRE(world.skorneArena().size() == 22);
    const auto* mark = world.layout().findLocator(LocatorKind::Boss);
    REQUIRE(mark != nullptr);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, -9.75f, 31}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 42);
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
    for (s32 frame = 0; frame < 600; ++frame) {
        world.update(1.0f / 30);
        opponents.update(2, 1.0f / 30, players, {}, events);
        effects.update(1.0f / 30);
    }
    REQUIRE(world.skorneArena().phase() == 3);
    opponents.close();
}

TEST_CASE("Skorne health and range windows expose his authored attack families",
          "[skorne][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/SKORNE1.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/SKORNE1/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'E');
    REQUIRE(fixture.spawn("SKORNE1", {0, -25.375f, 0}, 0));
    std::set<std::string> attacks;
    std::set<s32> shots;
    bool detachedSlam = false;
    for (const f32 health : {1.0f, 0.6f, 0.15f}) {
        for (const Vec3 target :
             {Vec3{0, -9.75f, 15}, Vec3{0, -9.75f, 33}, Vec3{0, -9.75f, 40}, Vec3{0, -9.75f, 65},
              Vec3{-18, -9.75f, 18}, Vec3{18, -9.75f, 18}}) {
            REQUIRE(fixture.actor.spawn(fixture.assets, 0, {0, -25.375f, 0}, 0, nullptr, {}, 'E'));
            if (health < 1) {
                EnemyHit hit;
                hit.damage = fixture.actor.maxHealth() * (1 - health);
                fixture.actor.hurt(hit);
            }
            EnemyView player;
            player.player = 0;
            player.position = target;
            player.height = 6;
            player.radius = 1;
            const std::array players{player};
            for (s32 frame = 0; frame < 5400; ++frame) {
                fixture.update(2, 1.0f / 30, players);
                if (fixture.actor.moveType() >= 128) {
                    attacks.emplace(fixture.actor.moveName());
                }
                for (const auto& shot : fixture.actor.takeShots()) {
                    shots.insert(shot.damageIndex);
                }
                for (const auto& cue : fixture.actor.takeCues()) {
                    if (cue.tree == "BOSSATK6FX" || cue.tree == "BOSSATK7FX") {
                        REQUIRE_FALSE(cue.follows);
                        REQUIRE(cue.placement.has_value());
                        REQUIRE(cue.life > 0);
                        detachedSlam = true;
                    }
                }
                fixture.actor.takeBlows();
            }
        }
    }
    REQUIRE(attacks == std::set<std::string>{"CLAWL", "GRAB", "FBALL_L", "FBALL_R", "EYEBEAM",
                                             "STOMP", "FOUNTAIN", "WING_ATTACK"});
    REQUIRE(shots == std::set<s32>{4, 5});
    REQUIRE(detachedSlam);
}
} // namespace
