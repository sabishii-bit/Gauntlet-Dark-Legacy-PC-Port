#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Death events retain their birth through slot reuse and eight tick native rewind",
          "[death][local-rewind][native-combat-audio]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const EnemySpawn spawn{.kind = kDeathKind,
                           .placed = true,
                           .asleep = true,
                           .priority = EnemySpawn::Priority::Visible};
    REQUIRE(enemies.spawn(spawn, {}) == 0);
    const auto birth = enemies.observe(0)->instance;
    auto saved = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(enemies.saveLocal(*saved));
    const auto advance = [&](s32 tick, bool wrong) {
        if (tick == 0) {
            enemies.hurt(0, EnemyHit{.damage = 1, .player = 0});
        } else if (tick == 1) {
            enemies.hurt(0, EnemyHit{.damage = 1, .player = 0, .antiDeath = !wrong});
        } else if (tick == 2) {
            enemies.hurt(0, EnemyHit{.flags = EnemyHit::kMagic, .player = 0, .level = 99});
        } else if (tick == 3) {
            REQUIRE(enemies.spawn(spawn, {}) == 0);
            enemies.hurt(0, EnemyHit{.damage = 1, .player = 0});
        } else if (tick == 4) {
            enemies.finishDeath(0, 0);
        }
        enemies.update(1, 1.0f / 60, {});
        return enemies.takeDeathEvents();
    };
    std::array<std::vector<DeathEvent>, 8> expected;
    const auto states = std::make_unique<std::array<Enemies::LocalSnapshot, 8>>();
    for (s32 tick = 0; tick < 8; ++tick) {
        expected[tick] = advance(tick, false);
        REQUIRE(enemies.saveLocal((*states)[tick]));
        for (const auto& event : expected[tick]) {
            CHECK(event.enemy == 0);
            CHECK(event.instance == (tick < 3 ? birth : birth + 1));
        }
    }
    REQUIRE(expected[0].size() == 1);
    CHECK(expected[0][0].kind == DeathEvent::Kind::Awakened);
    REQUIRE(expected[2].size() == 2);
    CHECK(expected[2][0].kind == DeathEvent::Kind::MagicHeal);
    CHECK(expected[2][0].amount > 0);
    CHECK(expected[2][1].kind == DeathEvent::Kind::Killed);
    REQUIRE(expected[4].size() == 1);
    CHECK(expected[4][0].kind == DeathEvent::Kind::Exhausted);
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(enemies.restoreLocal(*saved));
        for (s32 tick = 0; tick < distance; ++tick) {
            advance(tick, true);
        }
        REQUIRE(enemies.restoreLocal(*saved));
        for (s32 tick = 0; tick < distance; ++tick) {
            CHECK(advance(tick, false) == expected[tick]);
            const auto now = std::make_unique<Enemies::LocalSnapshot>();
            REQUIRE(enemies.saveLocal(*now));
            CHECK(enemies.equalLocal(*now, (*states)[tick]));
        }
    }
}

TEST_CASE("Death reverses its seek route after nine world-blocked movement steps",
          "[death][enemies][alpha-seek-blocked]") {
    // do_enemy_collide 80045488's blocked00 counts actual stops, including steps
    // during the hold: five ticks on an ordinary route, reversed after nine stops.
    const s32 ticks = GENERATE(1, 2);
    const f32 radius = enemyKind(kDeathKind).radius;
    const auto triangle = [](Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
        return CollisionTriangle{.normal = normal, .vertices = {a, b, c}};
    };
    WorldCollision collision;
    collision.build({triangle({-40, 0, -40}, {40, 0, 40}, {40, 0, -40}, {0, 1, 0}),
                     triangle({-40, 0, -40}, {-40, 0, 40}, {40, 0, 40}, {0, 1, 0}),
                     triangle({-40, 0, radius}, {40, 8, radius}, {40, 0, radius}, {0, 0, -1}),
                     triangle({-40, 0, radius}, {-40, 8, radius}, {40, 8, radius}, {0, 0, -1}),
                     triangle({radius, 0, -40}, {radius, 8, 40}, {radius, 0, 40}, {-1, 0, 0}),
                     triangle({radius, 0, -40}, {radius, 8, -40}, {radius, 8, 40}, {-1, 0, 0})});
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), &collision, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto id = enemies.spawn({.kind = kDeathKind, .tier = 1, .placed = true}, {});
    REQUIRE(id);
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 20}}};
    const f32 seconds = static_cast<f32>(ticks) / 60;
    for (s32 frame = 0; frame < 240 && !enemies.blockedOf(*id); ++frame) {
        enemies.update(ticks, seconds, players);
    }
    REQUIRE(enemies.blockedOf(*id));
    REQUIRE(enemies.bumpedWallOf(*id));
    const Vec3 blocked = enemies.positionOf(*id);
    CHECK(enemies.memoryOf(*id).deadEnd == 5);
    CHECK(enemies.memoryOf(*id).collided == 1);
    CHECK(enemies.memoryOf(*id).route == 1);
    // Stop Time still resolves contact, but must not count it as an AI step.
    for (s32 frame = 0; frame < 10; ++frame) {
        enemies.update(ticks, seconds, players, {}, nullptr, 1, true);
    }
    CHECK(enemies.memoryOf(*id).deadEnd == 5);
    CHECK(enemies.memoryOf(*id).collided == 1);
    for (s32 stop = 2; stop <= 9; ++stop) {
        for (s32 tick = 0; tick < 2; tick += ticks) {
            enemies.update(ticks, seconds, players);
        }
        CAPTURE(stop, ticks);
        REQUIRE(enemies.blockedOf(*id));
        CHECK(glm::distance(enemies.positionOf(*id), blocked) < 0.001f);
        CHECK(enemies.memoryOf(*id).collided == (stop == 9 ? 0 : stop));
        CHECK(enemies.memoryOf(*id).route == (stop == 9 ? -2 : 1));
    }
    // The left side is open. The reversed sweep must eventually take it, rather
    // than searching only the obstructed right side forever.
    for (s32 frame = 0; frame < 120 && enemies.positionOf(*id).x > -0.1f; ++frame) {
        enemies.update(ticks, seconds, players);
    }
    CHECK(enemies.positionOf(*id).x < -0.1f);
}

TEST_CASE("Death holds its seek route after hitting another enemy body",
          "[death][enemies][alpha-seek-blocked]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto walker = enemies.spawn({.kind = kDeathKind, .tier = 1, .placed = true}, {});
    REQUIRE(walker);
    const auto blocker = enemies.spawn({.kind = kDeathKind,
                                        .tier = 1,
                                        .position = {0.1f, 0, 0.1f},
                                        .placed = true,
                                        .asleep = true},
                                       {});
    REQUIRE(blocker);
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 20}}};
    enemies.update(2, 1.0f / 30, players);
    REQUIRE(enemies.blockedOf(*walker));
    CHECK_FALSE(enemies.bumpedWallOf(*walker));
    // do_enemy_move 80044664 takes the side away from the body and holds it
    // for sixty ticks; the body must not be mistaken for a five-tick world stop.
    CHECK(enemies.memoryOf(*walker).deadEnd == 60);
    CHECK(enemies.memoryOf(*walker).route == 1);
    enemies.update(2, 1.0f / 30, players);
    CHECK(enemies.memoryOf(*walker).deadEnd == 58);
    CHECK(enemies.positionOf(*walker) == Vec3{0, 0, 0});
}

TEST_CASE("Enemies traverse the Temple's altar stairs without oscillating between treads",
          "[death][enemies][assets][death-stairs]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    for (usize i = 0; i < world.triggers().size(); ++i) {
        world.activateTrigger(world.triggers().trigger(i).id, WorldAnimator::Finish::AtOnce);
    }
    Enemies enemies;
    enemies.open(device, root, &world.collision(), 1, {}, 1);
    const auto kind = GENERATE(kDeathKind, kGruntKind);
    const bool down = GENERATE(false, true);
    CAPTURE(kind, down);
    REQUIRE(enemies.loadKind(kind));
    const auto slot = enemies.spawn({.kind = kind,
                                     .tier = 1,
                                     .position = down ? Vec3{0, 5.0234375f, -94} : Vec3{0, 0, -80},
                                     .placed = true},
                                    {});
    REQUIRE(slot);
    std::array<EnemyView, 1> players{
        EnemyView{.player = 0, .position = down ? Vec3{0, 0, -70} : Vec3{0, 5.0234375f, -105}}};
    f32 ascent = 0;
    f32 descent = 0;
    f32 retreat = 0;
    for (s32 tick = 0; tick < 900; ++tick) {
        const Vec3 before = enemies.positionOf(*slot);
        enemies.update(2, 1.0f / 30, players);
        const Vec3 delta = enemies.positionOf(*slot) - before;
        ascent += std::max(delta.y, 0.0f);
        descent += std::max(-delta.y, 0.0f);
        retreat += std::max(down ? -delta.z : delta.z, 0.0f);
        if (down ? enemies.positionOf(*slot).z > -80 : enemies.positionOf(*slot).z < -92.5f) {
            break;
        }
    }
    const Vec3 at = enemies.positionOf(*slot);
    INFO("Enemy position " << at.x << ", " << at.y << ", " << at.z);
    // A final position alone misses the alternating uphill/downhill snaps that
    // occurred when the mind's wall probe disagreed with body movement.
    CHECK((down ? ascent : descent) < 0.01f);
    if (down) {
        CHECK(retreat < 0.01f);
    }
    CHECK((down ? at.z > -80 : at.z < -92.5f));
    CHECK(at.y == Catch::Approx(down ? 0 : 5).margin(0.1));
}

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
    // Struck without protection or magic, it is unmoved, and the striker is told (msgPost 0).
    const auto unmoved = enemies.takeDeathEvents();
    REQUIRE(unmoved.size() == 1);
    CHECK(unmoved[0].kind == DeathEvent::Kind::Unmoved);
    CHECK(unmoved[0].player == 3);
    hit.antiDeath = true;
    enemies.hurt(*slot, hit);
    auto cues = enemies.takeDeathEvents();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].kind == DeathEvent::Kind::Return);
    CHECK(cues[0].amount == (tier == 2 ? 46 : 1));
    hit.flags = EnemyHit::kMagic;
    CHECK(enemies.hurt(*slot, hit) == 0); // Death's separate MagicHeal event is not shared healing
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

TEST_CASE("Death flees the nearest protected player in three dimensions",
          "[death][enemies][multiplayer]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto slot = enemies.spawn({.kind = kDeathKind, .tier = 1, .placed = true}, {});
    REQUIRE(slot);
    const std::array players{EnemyView{.player = 3, .position = {0, 40, 1}, .antiDeath = true},
                             EnemyView{.player = 1, .position = {20, 0, 0}, .antiDeath = true}};
    for (s32 frame = 0; frame < 45; ++frame) {
        enemies.update(2, 1.0f / 30, players);
    }
    CHECK(enemies.targetOf(*slot) == -1);
    CHECK(enemies.positionOf(*slot).x < -1);
    CHECK(enemies.positionOf(*slot).z == Catch::Approx(0).margin(0.001));
}

TEST_CASE("Anti Death holds movement and outgoing drain only while a live contact remains",
          "[death][enemies][death-hold][multiplayer]") {
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto slot = enemies.spawn({.kind = kDeathKind, .tier = 1, .placed = true}, {});
    REQUIRE(slot);
    std::array players{EnemyView{.player = 3, .position = {0, 0, 2}, .antiDeath = true},
                       EnemyView{.player = 0, .position = {15, 0, 0}}};
    // Clear placement stun before the hold, so a stuck body cannot hide an AI error.
    for (s32 elapsed = 0; elapsed < 90; elapsed += ticks) {
        enemies.update(ticks, seconds, players);
    }
    const Vec3 held = enemies.positionOf(*slot);
    players[0].position = held + Vec3{0, 0, 2};
    players[0].heldDeath = *slot;
    for (s32 elapsed = 0; elapsed < 60; elapsed += ticks) {
        enemies.update(ticks, seconds, players);
        CHECK(enemies.positionOf(*slot) == held);
        CHECK_FALSE(enemies.draining(*slot));
    }
    CHECK(enemies.takeDeathEvents().empty());
    const f32 heldHeading = enemies.memoryOf(*slot).seekHeading;
    SECTION("unequipping releases even a stale held slot") {
        players[0].antiDeath = false;
    }
    SECTION("losing contact releases even a stale held slot") {
        players[0].position += Vec3{0, 0, 10};
    }
    SECTION("losing height overlap releases even a stale held slot") {
        players[0].position.y += 20;
    }
    SECTION("falling releases even a stale held slot") {
        players[0].hidden = true;
    }
    SECTION("being captured releases even a stale held slot") {
        players[0].captured = true;
    }
    SECTION("no held target releases") {
        players[0].heldDeath = -1;
    }
    SECTION("another holder keeps Death held after the first releases") {
        players[1].position = held + Vec3{2, 0, 0};
        players[1].antiDeath = true;
        players[1].heldDeath = *slot;
        players[0].heldDeath = -1;
        for (s32 elapsed = 0; elapsed < 30; elapsed += ticks) {
            enemies.update(ticks, seconds, players);
            CHECK(enemies.positionOf(*slot) == held);
        }
        players[1].heldDeath = -1;
    }
    // Change the unprotected target's bearing. Even where the old wearer's body
    // still blocks movement, one native update must resume thinking about it.
    players[1].position = held + Vec3{-10, 0, 0};
    players[1].antiDeath = false;
    for (s32 elapsed = 0; elapsed < 2; elapsed += ticks) {
        enemies.update(ticks, seconds, players);
    }
    CHECK((enemies.positionOf(*slot) != held || enemies.draining(*slot) ||
           enemies.memoryOf(*slot).seekHeading != heldHeading));
}

TEST_CASE("holding Death does not preserve an expired placement stun", "[death][death-hold]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::deathArchive(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kDeathKind));
    const auto slot = enemies.spawn({.kind = kDeathKind, .tier = 1, .placed = true}, {});
    REQUIRE(slot);
    std::array players{
        EnemyView{.player = 0, .position = {0, 0, 2}, .antiDeath = true, .heldDeath = *slot}};
    const Vec3 before = enemies.positionOf(*slot);
    for (s32 frame = 0; frame < 30; ++frame) {
        enemies.update(2, 1.0f / 30, players);
    }
    CHECK(enemies.positionOf(*slot) == before);
    players[0].position.z = 12;
    players[0].antiDeath = false;
    enemies.update(2, 1.0f / 30, players);
    CHECK(enemies.positionOf(*slot) != before);
}

TEST_CASE("retail Death bodies and drain effects are present", "[death][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/DEATH/ANIM.PS2").parent_path().parent_path().parent_path();
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
