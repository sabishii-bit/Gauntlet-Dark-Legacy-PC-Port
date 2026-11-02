#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Checkpoint = EnemyMissiles::Checkpoint;

EnemyView player(const Vec3& feet, s32 seat, bool reflects = false) {
    EnemyView view;
    view.player = seat;
    view.position = feet;
    view.reflects = reflects;
    return view;
}

Checkpoint capture(const EnemyMissiles& pool,
                   std::span<const EnemyMissiles::Resource> resources = {}) {
    const auto state = pool.checkpoint(resources);
    REQUIRE(state);
    return *state;
}

struct Battle {
    EnemyMissiles pool{125};
    std::array<EnemyView, 4> players{player({0, 0, 0.5f}, 0), player({-20, 0, 5}, 1, true),
                                     player({12, 0, 0}, 2), player({13, 0, 0}, 3)};
    std::array<MissileTarget, 2> swarm{MissileTarget{7, {-20, 0, -6}, 1, 6},
                                       MissileTarget{9, {12, 0, 1}, 1, 6}};
    usize callbacks = 0;

    Battle() {
        pool.setShrink(0.667f);
        auto held = EnemyMissileKind::deathShot();
        held.held = 0.437f;
        pool.launch(held, {0, 3, 0}, {0, 3, 60}, 1, nullptr, 5);
        pool.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {-20, 3, 0}, {-20, 3, 20}, 1, nullptr, 2);
        pool.launch(EnemyMissileKind::bomb(), {30, 8, 0}, {30, 0, 25}, 1, nullptr, 3);
        EnemyBlast gas;
        gas.position = {12, 3, 0};
        gas.radius = 7.5f;
        gas.damage = 10;
        gas.flags = EnemyBlast::kGas;
        gas.stages = {0.27f, 0.43f, 0.33f};
        gas.spared = 2;
        gas.lit = true;
        pool.blast(gas);
    }

    void advance(f32 seconds) {
        pool.update(seconds, nullptr, players, swarm, {},
                    [&](const PickupBlastReach&, std::vector<s32>& touched) {
                        ++callbacks;
                        if (std::ranges::find(touched, 42) == touched.end()) {
                            touched.push_back(42);
                        }
                    });
    }

    void step(s32 tick, f32 seconds) {
        if (tick % 9 == 0) {
            EnemyMissileLaunch launch;
            launch.body = {50, 3, 0};
            launch.target = {50, 3, 20};
            launch.aimError = 0.35f;
            launch.facing = tick % 18 == 0 ? kPi : 0;
            pool.launch(EnemyMissileKind::bomb(), launch);
        }
        pool.setShrink(tick >= 7 && tick < 19 ? 0.5f : 1);
        advance(seconds);
    }
};

TEST_CASE("native enemy launches reproduce decisions and random consumption",
          "[rollback][enemy-missiles-checkpoint][enemy-launch]") {
    EnemyMissiles native{891};
    EnemyMissiles replay{891};
    WorldCollision collision;
    collision.build({});
    Obstacle box;
    box.centre = {0, 0, 1.5f};
    box.halfAcross = 2;
    box.halfAlong = 0.25f;
    box.height = 10;
    const std::array items{box};
    EnemyMissileLaunch launch;
    launch.body = {0, 3, 0};
    launch.target = {0, 3, 20};
    launch.point = enemyLaunchPointOf(kGruntKind, EnemyMissileKind::kArrow);
    launch.aimError = 0.25f;
    const auto kind = GENERATE(EnemyMissileKind::arrow(), EnemyMissileKind::bomb(),
                               EnemyMissileKind::bolt(15, 25, 0.3f));
    for (s32 step = 0; step < 6; ++step) {
        CAPTURE(step, kind.slot);
        launch.facing = step % 3 == 1 ? kPi : 0;
        const auto obstacles =
            step % 3 == 2 ? std::span<const Obstacle>{items} : std::span<const Obstacle>{};
        const bool expected = native.launch(kind, launch, &collision, obstacles);
        CHECK(replay.launch(kind, launch, &collision, obstacles) == expected);
        CHECK(capture(replay) == capture(native));
    }
    CHECK(native.count() == 2);
}

TEST_CASE("whole enemy missile replay retains held flight reflection piercing bursts and RNG",
          "[rollback][enemy-missiles-checkpoint]") {
    const f32 step = GENERATE(1.0f / 30, 1.0f / 60);
    Battle battle;
    battle.advance(0.173f); // neither a 30 Hz nor a 60 Hz boundary
    const auto baseline = capture(battle.pool);
    REQUIRE(baseline.missiles.size() == 3);
    CHECK(baseline.missiles[0].heldLeft > 0);
    CHECK_FALSE(baseline.missiles[0].pierced.empty());
    CHECK(baseline.missiles[1].reflected);
    CHECK(baseline.missiles[1].continuity > 1);
    CHECK(baseline.ricochetIn > 0);
    REQUIRE(baseline.bursts.size() == 1);
    CHECK(baseline.bursts[0].players[0].secondsLeft == std::numeric_limits<f32>::infinity());
    CHECK_FALSE(baseline.bursts[0].swarm.empty());
    CHECK(baseline.bursts[0].scenery == std::vector<s32>{42});
    CHECK_FALSE(baseline.hits.empty());
    CHECK_FALSE(baseline.pickupBlasts.empty());

    std::vector<Checkpoint> forward;
    for (s32 tick = 0; tick < 96; ++tick) {
        battle.step(tick, step);
        forward.push_back(capture(battle.pool));
    }
    CHECK_FALSE(forward.back().random == baseline.random);
    CHECK(forward.back().nextInstance > baseline.nextInstance);
    const auto calls = battle.callbacks;
    auto prepared = battle.pool.prepareRestore(baseline);
    REQUIRE(prepared);
    CHECK(battle.callbacks == calls);
    CHECK(capture(battle.pool) == forward.back());
    REQUIRE(battle.pool.canCommit(*prepared));
    REQUIRE(battle.pool.commitRestore(std::move(*prepared)));
    CHECK(battle.callbacks == calls);
    CHECK(capture(battle.pool) == baseline);
    for (s32 tick = 0; tick < 96; ++tick) {
        CAPTURE(step, tick);
        battle.step(tick, step);
        CHECK(capture(battle.pool) == forward[static_cast<usize>(tick)]);
    }
}

TEST_CASE("enemy missile checkpoints preserve undelivered rock damage and every output queue",
          "[rollback][enemy-missiles-checkpoint]") {
    EnemyMissiles pool;
    Obstacle cover;
    cover.centre = {0, 0, 5};
    cover.halfAcross = 2;
    cover.halfAlong = 2;
    cover.height = 6;
    const std::array items{MissileStop{.box = cover, .rock = 3, .rockHealth = 40}};
    const auto kind = *enemyMissileOf(kGarmBroodKind, EnemyMissileKind::kBolt);
    pool.launch(kind, {0, 3, 0}, {0, 3, 40}, 1, nullptr, 2);
    pool.update(0.1f, nullptr, {}, {}, items);
    auto baseline = capture(pool);
    REQUIRE(baseline.rockHits.size() == 1);
    REQUIRE(baseline.hits.size() == 1);
    REQUIRE(baseline.missiles.empty());
    pool.burst({20, 3, 0}, 4, 10, EnemyMissileKind::kKnockBack, 1, -1);
    pool.update(1.0f / 60, nullptr, std::array{player({20, 0, 0}, 0)}, {}, items);
    baseline = capture(pool);
    REQUIRE_FALSE(baseline.pickupBlasts.empty());

    const auto follow = [&] {
        pool.launch(kind, {0, 3, 0}, {0, 3, 40}, 1, nullptr, 2);
        pool.update(0.2f, nullptr, {}, {}, items);
        REQUIRE(pool.count() == 1);
        CHECK(pool.missile(0).position.z > 10);
        const auto result = capture(pool);
        REQUIRE(result.rockHits.size() == 2); // pending first impact makes the second pierce
        return result;
    };
    const auto forward = follow();
    REQUIRE(pool.restore(baseline));
    CHECK(follow() == forward);
    CHECK(pool.takeHits() == forward.hits);
    CHECK(pool.takeRockHits() == forward.rockHits);
    CHECK(pool.takePickupBlasts() == forward.pickupBlasts);
    CHECK(pool.takeHits().empty());
    CHECK(pool.takeRockHits().empty());
    CHECK(pool.takePickupBlasts().empty());
    REQUIRE(pool.restore(forward));
    CHECK(capture(pool) == forward);
    CHECK(pool.takeHits() == forward.hits);
    CHECK(pool.takeRockHits() == forward.rockHits);
    CHECK(pool.takePickupBlasts() == forward.pickupBlasts);
}

TEST_CASE(
    "enemy missile RNG advances even for rejected launches and clear preserves native counters",
    "[rollback][enemy-missiles-checkpoint]") {
    EnemyMissiles pool{771};
    const auto original = capture(pool);
    EnemyMissileLaunch launch;
    launch.body = {0, 3, 0};
    launch.target = {0, 3, 20};
    launch.facing = kPi;
    launch.aimError = 0.8f;
    CHECK_FALSE(pool.launch(EnemyMissileKind::arrow(), launch));
    const auto backwards = capture(pool);
    CHECK_FALSE(backwards.random == original.random);
    CHECK(backwards.nextInstance == original.nextInstance);
    launch.facing = 0;
    Obstacle wall;
    wall.centre = {0, 0, 1};
    wall.halfAcross = 2;
    wall.halfAlong = 2;
    wall.height = 20;
    CHECK_FALSE(pool.launch(EnemyMissileKind::arrow(), launch, nullptr, std::array{wall}));
    const auto blocked = capture(pool);
    CHECK_FALSE(blocked.random == backwards.random);
    REQUIRE(pool.launch(EnemyMissileKind::arrow(), launch));
    const auto forward = capture(pool);
    REQUIRE(pool.restore(blocked));
    REQUIRE(pool.launch(EnemyMissileKind::arrow(), launch));
    CHECK(capture(pool) == forward);
    pool.blast({.position = {70, 3, 0}, .stages = {1}});
    const auto afterBlast = capture(pool);
    CHECK(afterBlast.random == forward.random);
    CHECK(afterBlast.bursts.back().instance == forward.nextInstance);
    CHECK(afterBlast.nextInstance == forward.nextInstance + 1);
    pool.setShrink(0.5f);
    const auto beforeClear = capture(pool);
    pool.clear();
    const auto cleared = capture(pool);
    CHECK(cleared.random == beforeClear.random);
    CHECK(cleared.nextInstance == beforeClear.nextInstance);
    CHECK(cleared.shrink == beforeClear.shrink);
    CHECK(cleared.ricochetIn == beforeClear.ricochetIn);
    CHECK(cleared.missiles.empty());
}

TEST_CASE("restored enemy lobs retain exact world impacts and liquid collision ownership",
          "[rollback][enemy-missiles-checkpoint]") {
    const bool water = GENERATE(false, true);
    const f32 step = GENERATE(1.0f / 30, 1.0f / 60);
    std::vector<CollisionTriangle> floor(2);
    floor[0].vertices = {Vec3{-60, 0, -60}, Vec3{60, 0, -60}, Vec3{60, 0, 60}};
    floor[1].vertices = {Vec3{-60, 0, -60}, Vec3{60, 0, 60}, Vec3{-60, 0, 60}};
    for (auto& triangle : floor) {
        triangle.normal = {0, 1, 0};
        triangle.object = 73;
        triangle.objectFlags = water ? WorldCollision::kLiquidSurface : WorldObject::kFloor;
    }
    WorldCollision collision;
    collision.build(floor);
    EnemyMissiles pool;
    pool.launch(EnemyMissileKind::bomb(), {0, 4, 0}, {0, 0, 10}, 1, nullptr, 2);
    pool.update(0.137f, &collision, {});
    const auto baseline = capture(pool);
    REQUIRE(baseline.missiles.size() == 1);
    std::vector<Checkpoint> forward;
    for (usize tick = 0; tick < 120; ++tick) {
        pool.update(step, &collision, {});
        forward.push_back(capture(pool));
    }
    REQUIRE(forward.back().hits.size() == 1);
    CHECK(forward.back().hits[0].worldContact);
    CHECK(forward.back().hits[0].worldObject == 73);
    CHECK(forward.back().hits[0].liquid == water);
    REQUIRE(pool.restore(baseline));
    for (usize tick = 0; tick < forward.size(); ++tick) {
        CAPTURE(water, step, tick);
        pool.update(step, &collision, {});
        CHECK(capture(pool) == forward[tick]);
    }
}

TEST_CASE("enemy missile prepared restore is owner-bound single-use and invalidated by clear",
          "[rollback][enemy-missiles-checkpoint]") {
    std::optional<EnemyMissiles> pool{std::in_place};
    pool->launch(EnemyMissileKind::bomb(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 2);
    const auto baseline = capture(*pool);
    auto prepared = pool->prepareRestore(baseline);
    auto sibling = pool->prepareRestore(baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    EnemyMissiles other;
    const auto otherBefore = capture(other);
    CHECK_FALSE(other.canCommit(*prepared));
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    CHECK(capture(other) == otherBefore);
    REQUIRE(pool->canCommit(*prepared));
    EnemyMissiles::Prepared moved = std::move(*prepared);
    CHECK_FALSE(pool->canCommit(*prepared));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    REQUIRE(pool->commitRestore(std::move(moved)));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(pool->canCommit(moved));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(pool->commitRestore(std::move(moved)));
    CHECK_FALSE(pool->canCommit(*sibling));
    CHECK(capture(*pool) == baseline);

    prepared = pool->prepareRestore(baseline);
    REQUIRE(prepared);
    pool->clear();
    CHECK_FALSE(pool->canCommit(*prepared));
    CHECK_FALSE(pool->commitRestore(std::move(*prepared)));
    prepared = pool->prepareRestore(baseline);
    REQUIRE(prepared);
    // An object reconstructed at the same address is not the original owner.
    const auto* address = &*pool;
    pool.emplace();
    CHECK(&*pool == address);
    CHECK_FALSE(pool->canCommit(*prepared));
    CHECK_FALSE(pool->commitRestore(std::move(*prepared)));
}

TEST_CASE("enemy missile malformed checkpoints and bounded overflows leave the live pool intact",
          "[rollback][enemy-missiles-checkpoint]") {
    Battle battle;
    battle.advance(0.173f);
    const auto baseline = capture(battle.pool);
    auto validPreparation = battle.pool.prepareRestore(baseline);
    REQUIRE(validPreparation);
    const auto reject = [&](const Checkpoint& bad) {
        CHECK_FALSE(battle.pool.prepareRestore(bad));
        CHECK_FALSE(battle.pool.restore(bad));
        CHECK(capture(battle.pool) == baseline);
        CHECK(battle.pool.canCommit(*validPreparation));
    };
    auto bad = baseline;
    bad.missiles[0].position.x = std::numeric_limits<f32>::quiet_NaN();
    reject(bad);
    bad = baseline;
    bad.missiles[1].instance = bad.missiles[0].instance;
    reject(bad);
    bad = baseline;
    bad.missiles[0].instance = bad.nextInstance;
    reject(bad);
    bad = baseline;
    bad.missiles[0].continuity = 0;
    reject(bad);
    bad = baseline;
    bad.missiles[0].resource = 900;
    reject(bad);
    bad = baseline;
    bad.missiles[0].pierced.emplace_back(0, EnemyMissiles::kPierceGap);
    reject(bad);
    bad = baseline;
    bad.missiles[0].kind.slot = 4;
    reject(bad);
    bad = baseline;
    bad.missiles[0].heldLeft = bad.missiles[0].kind.held + 1;
    reject(bad);
    bad = baseline;
    bad.missiles[0].secondsLeft = 0;
    reject(bad);
    bad = baseline;
    bad.bursts[0].stage = bad.bursts[0].blast.stages.size();
    reject(bad);
    bad = baseline;
    bad.bursts[0].blast.stages[0] = 0;
    reject(bad);
    bad = baseline;
    bad.bursts[0].players[0].secondsLeft = 1;
    reject(bad);
    bad = baseline;
    bad.bursts[0].players.push_back({0, std::numeric_limits<f32>::infinity()});
    reject(bad);
    bad = baseline;
    bad.bursts[0].swarm[0].secondsLeft = std::numeric_limits<f32>::infinity();
    reject(bad);
    bad = baseline;
    bad.hits.back().player = 4;
    reject(bad);
    bad = baseline;
    bad.pickupBlasts.back().radius = -1;
    reject(bad);
    bad = baseline;
    bad.rockHits.push_back({0, std::numeric_limits<f32>::quiet_NaN()});
    reject(bad);
    bad = baseline;
    bad.shrink = 0;
    reject(bad);
    bad = baseline;
    bad.ricochetIn = -1;
    reject(bad);
    bad = baseline;
    bad.missiles.resize(Checkpoint::kMaxMissiles + 1);
    reject(bad);
    bad = baseline;
    bad.bursts.resize(Checkpoint::kMaxBursts + 1);
    reject(bad);
    bad = baseline;
    bad.hits.resize(Checkpoint::kMaxEvents + 1);
    reject(bad);
    bad = baseline;
    bad.bursts[0].scenery.resize(Checkpoint::kMaxContacts + 1);
    reject(bad);
    bad = baseline;
    bad.bursts[0].blast.stages.resize(Checkpoint::kMaxStages + 1, 1);
    reject(bad);
    CHECK(battle.pool.commitRestore(std::move(*validPreparation)));
}

TEST_CASE("enemy missile capture overflow refuses without pruning native missiles or outputs",
          "[rollback][enemy-missiles-checkpoint]") {
    EnemyMissiles pool;
    for (usize index = 0; index <= Checkpoint::kMaxMissiles; ++index) {
        pool.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, -1);
    }
    CHECK_FALSE(pool.checkpoint());
    CHECK(pool.count() == Checkpoint::kMaxMissiles + 1);
    CHECK(pool.missile(0).instance == 1);
    CHECK(pool.missile(pool.count() - 1).instance == Checkpoint::kMaxMissiles + 1);
    pool.clear();
    EnemyBlast longBlast;
    longBlast.stages.resize(Checkpoint::kMaxStages + 1, 1);
    pool.blast(std::move(longBlast));
    CHECK_FALSE(pool.checkpoint());
    CHECK(pool.burstCount() == 1);
}

TEST_CASE("enemy missile checkpoint preserves exhausted lifetime counters without reusing IDs",
          "[rollback][enemy-missiles-checkpoint]") {
    EnemyMissiles pool;
    auto state = capture(pool);
    state.nextInstance = std::numeric_limits<u64>::max();
    REQUIRE(pool.restore(state));
    pool.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, -1);
    const auto exhausted = capture(pool);
    CHECK(exhausted.nextInstance == 0);
    REQUIRE(exhausted.missiles.size() == 1);
    CHECK(exhausted.missiles[0].instance == std::numeric_limits<u64>::max());
    REQUIRE(pool.restore(exhausted));
    pool.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, -1);
    CHECK(capture(pool) == exhausted);
    EnemyMissileLaunch launch;
    launch.target = {0, 0, 20};
    CHECK_FALSE(pool.launch(EnemyMissileKind::arrow(), launch));
    CHECK(capture(pool) == exhausted);
}

TEST_CASE("enemy missile resources survive shooter recycling and resolve original loaded models",
          "[rollback][enemy-missiles-checkpoint][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 17);
    REQUIRE(enemies.loadKind(kGruntKind));
    REQUIRE(enemies.loadKind(13));
    const TreeModel* arrow = enemies.projectileModel(kGruntKind, 0);
    const TreeModel* otherArrow = enemies.projectileModel(13, 0);
    REQUIRE(arrow != nullptr);
    REQUIRE(otherArrow != nullptr);
    const std::array resources{EnemyMissiles::Resource{36864 + kGruntKind * 512, arrow},
                               EnemyMissiles::Resource{36864 + 13 * 512, otherArrow}};
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}) == 0);
    EnemyMissiles pool;
    pool.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, arrow, 0);
    const auto baseline = capture(pool, resources);
    CHECK(baseline.missiles[0].resource == resources[0].id);
    CHECK_FALSE(pool.checkpoint()); // unknown artwork must not become an invisible missile
    EnemyHit hit;
    hit.damage = 10000;
    hit.flags = EnemyHit::kMagic;
    enemies.hurt(0, hit);
    for (s32 tick = 0; tick < 600 && enemies.count() != 0; ++tick) {
        enemies.update(2, 1.0f / 30, {});
    }
    REQUIRE(enemies.count() == 0);
    spawn.kind = 13;
    REQUIRE(enemies.spawn(spawn, {}) == 0);
    const auto uploads = device.texturesCreated;
    pool.update(0.2f, nullptr, {});
    const auto forward = capture(pool, resources);
    REQUIRE(pool.restore(baseline, resources));
    CHECK(pool.missile(0).model == arrow);
    CHECK(pool.missile(0).model != otherArrow);
    CHECK(pool.missile(0).shooter == 0);
    pool.update(0.2f, nullptr, {});
    CHECK(capture(pool, resources) == forward);
    CHECK(device.texturesCreated == uploads);

    const auto reject = [&](std::span<const EnemyMissiles::Resource> invalid) {
        CHECK_FALSE(pool.prepareRestore(baseline, invalid));
        CHECK_FALSE(pool.restore(baseline, invalid));
        CHECK(capture(pool, resources) == forward);
        CHECK(device.texturesCreated == uploads);
    };
    reject({});
    auto bad = resources;
    bad[0].id = 0;
    reject(bad);
    bad = resources;
    bad[1].id = bad[0].id;
    reject(bad);
    bad = resources;
    bad[1].model = bad[0].model;
    reject(bad);
    bad = resources;
    bad[0].model = nullptr;
    reject(bad);

    TreeModel borrowed = *arrow;
    const std::array local{EnemyMissiles::Resource{resources[0].id, &borrowed}};
    auto prepared = pool.prepareRestore(baseline, local);
    REQUIRE(prepared);
    borrowed.clear();
    CHECK_FALSE(pool.canCommit(*prepared));
    CHECK_FALSE(pool.commitRestore(std::move(*prepared)));
    CHECK(capture(pool, resources) == forward);
    pool.clear(); // before the stock owner releases its resources
}

TEST_CASE("native enemy missile rewind discards wrong flight branches and preserves pending hits",
          "[enemy-missiles-local-rewind]") {
    Battle reference;
    Battle predicted;
    EnemyMissiles::LocalSnapshot saved;
    reference.advance(0.173f);
    predicted.advance(0.173f);
    bool sawHits = false;
    bool sawBursts = false;
    bool sawReflections = false;
    s32 tick = 0;
    for (s32 window = 0; window < 48; ++window) {
        CAPTURE(window, tick);
        const s32 distance = 1 + window % 8;
        const auto before = capture(predicted.pool);
        predicted.pool.saveLocal(saved);
        auto diagnostic = predicted.pool.prepareRestore(before);
        REQUIRE(diagnostic);
        for (s32 step = 0; step < distance; ++step) {
            predicted.step(tick + step + 999, 1.0f / 30);
        }
        predicted.pool.takeHits();
        predicted.pool.takeRockHits();
        predicted.pool.takePickupBlasts();
        const auto callbacks = predicted.callbacks;
        REQUIRE(predicted.pool.restoreLocal(saved));
        CHECK(predicted.callbacks == callbacks);
        CHECK_FALSE(predicted.pool.canCommit(*diagnostic));
        CHECK(capture(predicted.pool) == before);
        for (s32 step = 0; step < distance; ++step, ++tick) {
            reference.step(tick, 1.0f / 60);
            predicted.step(tick, 1.0f / 60);
            const auto expected = capture(reference.pool);
            CHECK(capture(predicted.pool) == expected);
            sawHits |= !expected.hits.empty();
            sawBursts |= !expected.bursts.empty();
            sawReflections |=
                std::ranges::any_of(expected.missiles, [](const auto& m) { return m.reflected; });
        }
        REQUIRE(predicted.pool.canRestoreLocal(saved));
    }
    CHECK(sawHits);
    CHECK(sawBursts);
    CHECK(sawReflections);
}

TEST_CASE("native enemy missile history refuses foreign and replaced bindings",
          "[enemy-missiles-local-rewind]") {
    Battle first;
    Battle other;
    EnemyMissiles::LocalSnapshot saved;
    CHECK_FALSE(first.pool.restoreLocal(saved));
    CHECK_FALSE(first.pool.equalLocal(saved, saved));
    first.pool.saveLocal(saved);
    CHECK(first.pool.equalLocal(saved, saved));
    const auto untouched = capture(other.pool);
    CHECK_FALSE(other.pool.restoreLocal(saved));
    CHECK_FALSE(other.pool.equalLocal(saved, saved));
    CHECK(capture(other.pool) == untouched);
    REQUIRE(first.pool.restoreLocal(saved));
    REQUIRE(first.pool.restoreLocal(saved));
    REQUIRE(first.pool.restore(capture(first.pool)));
    CHECK_FALSE(first.pool.restoreLocal(saved));
    CHECK_FALSE(first.pool.equalLocal(saved, saved));
    first.pool.saveLocal(saved);
    first.pool.clear();
    CHECK_FALSE(first.pool.restoreLocal(saved));
    CHECK_FALSE(first.pool.equalLocal(saved, saved));
}

TEST_CASE(
    "native enemy missile equality covers hidden contacts timers identity and borrowed models",
    "[enemy-missiles-local-rewind][native-state-equality]") {
    Battle battle;
    battle.advance(0.173f);
    EnemyMissiles::LocalSnapshot saved;
    EnemyMissiles::LocalSnapshot changed;
    battle.pool.saveLocal(saved);
    battle.pool.saveLocal(changed);
    REQUIRE(battle.pool.equalLocal(saved, changed));
    const auto distinct = [&](const auto& mutate) {
        REQUIRE(battle.pool.restoreLocal(saved));
        mutate();
        battle.pool.saveLocal(changed);
        CHECK_FALSE(battle.pool.equalLocal(saved, changed));
        REQUIRE(battle.pool.restoreLocal(saved));
        battle.pool.saveLocal(changed);
        CHECK(battle.pool.equalLocal(saved, changed));
    };
    const auto flight = [&]() -> EnemyMissile& {
        // This mutable pool owns the value; only its public observation is const.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        return const_cast<EnemyMissile&>(battle.pool.missile(0));
    };
    REQUIRE_FALSE(flight().pierced.empty());
    distinct([&] { flight().pierced[0].second *= 0.5f; });
    distinct([&] { ++flight().pierced[0].first; });
    distinct([&] { flight().heldLeft *= 0.5f; });
    distinct([&] { flight().secondsLeft *= 0.5f; });
    distinct([&] { flight().lived += 0.01f; });
    distinct([&] { ++flight().instance; });
    distinct([&] { ++flight().continuity; });
    distinct([&] { flight().reflected = !flight().reflected; });
    distinct([&] { flight().shooter = 17; });
    distinct([&] { flight().kind.damage += 1; });
    TreeModel otherBinding;
    distinct([&] { flight().model = &otherBinding; });
    distinct([&] { battle.pool.setShrink(0.5f); });
    distinct([&] { battle.pool.takeHits(); });
    distinct([&] { battle.pool.takePickupBlasts(); });
    distinct([&] {
        // Rejected throws still consume native RNG, without adding a body.
        const auto count = battle.pool.count();
        REQUIRE_FALSE(battle.pool.launch(
            EnemyMissileKind::bomb(),
            {.body = {50, 3, 0}, .target = {50, 3, 20}, .facing = kPi, .aimError = 0.35f}));
        REQUIRE(battle.pool.count() == count);
    });
    distinct([&] { battle.advance(1.0f / 60); }); // gas holds, stage and ricochet timers
}

TEST_CASE("enemy flight and lingering blast impacts retain births through compaction and rewind",
          "[enemy-missiles-local-rewind][native-combat-audio]") {
    EnemyMissiles pool;
    const auto kind = EnemyMissileKind::bolt(20, 25, 0.3f, 0, 0);
    pool.launch(kind, {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
    const u64 flight = pool.missile(0).instance;
    pool.blast({.position = {20, 3, 0},
                .radius = 7.5f,
                .damage = 10,
                .flags = EnemyBlast::kGas,
                .stages = {2}});
    const auto initial = capture(pool);
    REQUIRE(initial.bursts.size() == 1);
    const u64 blast = initial.bursts[0].instance;
    REQUIRE(blast != 0);
    REQUIRE(blast != flight);
    const std::array views{player({0, 0, 0.5f}, 0), player({20, 0, 0}, 1)};
    EnemyMissiles::LocalSnapshot saved;
    EnemyMissiles::LocalSnapshot expectedNative;
    EnemyMissiles::LocalSnapshot actualNative;
    pool.saveLocal(saved);
    const auto run = [&] {
        pool.update(1.0f / 60, nullptr, views);
        REQUIRE(pool.count() == 0);
        pool.launch(kind, {40, 3, 0}, {40, 3, 20}, 1, nullptr, 0);
        const u64 replacement = pool.missile(0).instance;
        REQUIRE(replacement != flight);
        REQUIRE(replacement != blast);
        const auto hits = pool.takeHits();
        REQUIRE(hits.size() == 2);
        CHECK(hits[0].fromBurst);
        CHECK(hits[0].instance == blast);
        CHECK_FALSE(hits[1].fromBurst);
        CHECK(hits[1].instance == flight);
        pool.update(0.51f, nullptr, views);
        const auto later = pool.takeHits();
        REQUIRE(later.size() == 1);
        CHECK(later[0].fromBurst);
        CHECK(later[0].instance == blast);
        return capture(pool);
    };
    const auto expected = run();
    pool.saveLocal(expectedNative);
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(pool.restoreLocal(saved));
        for (s32 tick = 0; tick < distance; ++tick) {
            pool.blast({.position = {80, 3, 0}, .stages = {1}});
            pool.update(1.0f / 60, nullptr, {});
        }
        REQUIRE(pool.restoreLocal(saved));
        CHECK(run() == expected);
        pool.saveLocal(actualNative);
        CHECK(pool.equalLocal(expectedNative, actualNative));
    }
}

} // namespace
