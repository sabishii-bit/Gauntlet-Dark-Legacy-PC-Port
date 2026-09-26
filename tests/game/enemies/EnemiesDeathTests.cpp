#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/enemies/Enemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Death contact drains on its clock, stops on separation, and expires without melee",
          "[death][enemies]") {
    const s32 tier = GENERATE(1, 2);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto slot = enemies.spawn({.kind = kDeathKind, .tier = tier, .placed = true}, {});
    REQUIRE(slot);
    std::array<EnemyView, 1> players{EnemyView{.player = 2, .position = {0, 0, 8}, .level = 30}};
    s32 drains = 0;
    s32 exhausted = 0;
    for (s32 frame = 0; frame < 300; ++frame) {
        enemies.update(2, 1.0f / 30, players);
        CHECK(enemies.takeBlows().empty());
        for (const auto& cue : enemies.takeDeathEvents()) {
            if (cue.kind == DeathEvent::Kind::Drain) {
                CHECK(cue.player == 2);
                CHECK(cue.amount == (tier == 2 ? 27 : 1));
                ++drains;
            } else if (cue.kind == DeathEvent::Kind::Exhausted) {
                ++exhausted;
            }
        }
        if (drains >= 10) {
            break;
        }
    }
    REQUIRE(drains == 10);
    CHECK(enemies.healthOf(*slot) == 90);
    players[0].position = {0, 0, 100};
    enemies.update(2, 1.0f / 30, players);
    CHECK_FALSE(enemies.draining(*slot));
    CHECK(enemies.takeDeathEvents().empty());
    players[0].position = enemies.positionOf(*slot) + Vec3{0, 0, 2};
    for (s32 frame = 0; frame < 300 && enemies.alive(*slot); ++frame) {
        enemies.update(2, 1.0f / 30, players);
        for (const auto& cue : enemies.takeDeathEvents()) {
            drains += cue.kind == DeathEvent::Kind::Drain ? 1 : 0;
            exhausted += cue.kind == DeathEvent::Kind::Exhausted ? 1 : 0;
        }
    }
    CHECK(drains == 101);
    CHECK(exhausted == 1);
    CHECK(enemies.dying(*slot));
    const f32 floor = enemies.positionOf(*slot).y;
    enemies.update(30, 0.5f, players);
    CHECK(enemies.positionOf(*slot).y == Catch::Approx(floor + 5));
    enemies.update(35, 35.0f / 60, players);
    CHECK(enemies.count() == 0);
    CHECK(enemies.takeLosses().empty());
}

TEST_CASE("Anti Death repels Death and returns its resource, while magic kills outright",
          "[death][enemies]") {
    const s32 tier = GENERATE(1, 2);
    test::FakeRenderDevice device;
    Enemies enemies;
    EnemyScales scales;
    scales.health = 20;
    enemies.open(device, test::deathArchive(), nullptr, 1, scales, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto slot = enemies.spawn({.kind = kDeathKind, .tier = tier, .placed = true}, {});
    REQUIRE(slot);
    CHECK(enemies.healthOf(*slot) == 100);
    std::array<EnemyView, 1> players{
        EnemyView{.player = 3, .position = {0, 0, 10}, .antiDeath = true}};
    for (s32 i = 0; i < 45; ++i) {
        enemies.update(2, 1.0f / 30, players);
    }
    CHECK(enemies.positionOf(*slot).z < 0);
    CHECK(enemies.targetOf(*slot) == -1);
    CHECK(enemies.takeDeathEvents().empty());
    EnemyHit hit;
    hit.player = 3;
    hit.level = 99;
    hit.damage = 999;
    enemies.hurt(*slot, hit);
    CHECK(enemies.healthOf(*slot) == 99);
    CHECK(enemies.takeFeedback().empty());
    CHECK(enemies.takeLosses().empty());
    hit.antiDeath = true;
    enemies.hurt(*slot, hit);
    auto cues = enemies.takeDeathEvents();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].kind == DeathEvent::Kind::Return);
    CHECK(cues[0].amount == (tier == 2 ? 46 : 1));
    hit.flags = EnemyHit::kMagic;
    enemies.hurt(*slot, hit);
    CHECK_FALSE(enemies.alive(*slot));
    cues = enemies.takeDeathEvents();
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].kind == DeathEvent::Kind::MagicHeal);
    CHECK(cues[0].amount == Catch::Approx(98 * 0.968f));
    CHECK(cues[1].kind == DeathEvent::Kind::Killed);
    const auto losses = enemies.takeLosses();
    REQUIRE(losses.size() == 1);
    CHECK(losses[0].experience == 0);
    CHECK(losses[0].killed);
}

TEST_CASE("retail Death bodies and drain effects are present", "[death][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/DEATH/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    CHECK(enemies.treeOf(kDeathKind, 1)->name == "DEATH1");
    CHECK(enemies.treeOf(kDeathKind, 2)->name == "DEATH2");
    REQUIRE(enemies.archiveOf(kDeathKind)->trees.find("DEATH_ARC"));
    REQUIRE(enemies.archiveOf(kDeathKind)->trees.find("DEATH_EXP"));
    enemies.close();
}
} // namespace
