#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;

EnemyView player(const Vec3& feet, s32 seat, bool reflects = false) {
    EnemyView view;
    view.player = seat;
    view.position = feet;
    view.reflects = reflects;
    return view;
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
          "[rollback][enemy-missiles-local-rewind][enemy-launch]") {
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
        CHECK(test::liveDigest(replay) == test::liveDigest(native));
    }
    CHECK(native.count() == 2);
}

TEST_CASE("whole enemy missile replay retains held flight reflection piercing bursts and RNG",
          "[rollback][enemy-missiles-local-rewind]") {
    const f32 step = GENERATE(1.0f / 30, 1.0f / 60);
    Battle battle;
    battle.advance(0.173f); // neither a 30 Hz nor a 60 Hz boundary
    EnemyMissiles::LocalSnapshot baseline;
    battle.pool.saveLocal(baseline);
    REQUIRE(battle.pool.count() == 3);
    CHECK(battle.pool.missile(0).heldLeft > 0);
    CHECK_FALSE(battle.pool.missile(0).pierced.empty());
    CHECK(battle.pool.missile(1).reflected);
    CHECK(battle.pool.missile(1).continuity > 1);
    REQUIRE(battle.pool.bursts().size() == 1);
    CHECK(battle.pool.bursts()[0].players[0].secondsLeft == std::numeric_limits<f32>::infinity());
    CHECK_FALSE(battle.pool.bursts()[0].swarm.empty());
    CHECK(battle.pool.bursts()[0].scenery == std::vector<s32>{42});
    CHECK_FALSE(battle.pool.takeHits().empty());
    CHECK_FALSE(battle.pool.takePickupBlasts().empty());
    REQUIRE(battle.pool.restoreLocal(baseline));

    std::vector<EnemyMissiles::LocalSnapshot> forward(96);
    for (s32 tick = 0; tick < 96; ++tick) {
        battle.step(tick, step);
        battle.pool.saveLocal(forward[static_cast<usize>(tick)]);
    }
    CHECK_FALSE(battle.pool.equalLocal(forward.back(), baseline));
    const auto calls = battle.callbacks;
    REQUIRE(battle.pool.restoreLocal(baseline));
    CHECK(battle.callbacks == calls);
    EnemyMissiles::LocalSnapshot current;
    battle.pool.saveLocal(current);
    CHECK(battle.pool.equalLocal(current, baseline));
    for (s32 tick = 0; tick < 96; ++tick) {
        CAPTURE(step, tick);
        battle.step(tick, step);
        battle.pool.saveLocal(current);
        CHECK(battle.pool.equalLocal(current, forward[static_cast<usize>(tick)]));
    }
}

TEST_CASE("enemy missile rewind preserves undelivered rock damage and every output queue",
          "[rollback][enemy-missiles-local-rewind]") {
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
    EnemyMissiles::LocalSnapshot baseline;
    pool.saveLocal(baseline);
    REQUIRE(pool.takeRockHits().size() == 1);
    REQUIRE(pool.takeHits().size() == 1);
    REQUIRE(pool.count() == 0);
    REQUIRE(pool.restoreLocal(baseline));
    pool.burst({20, 3, 0}, 4, 10, EnemyMissileKind::kKnockBack, 1, -1);
    pool.update(1.0f / 60, nullptr, std::array{player({20, 0, 0}, 0)}, {}, items);
    pool.saveLocal(baseline);
    REQUIRE_FALSE(pool.takePickupBlasts().empty());
    REQUIRE(pool.restoreLocal(baseline));

    const auto follow = [&](EnemyMissiles::LocalSnapshot& state) {
        pool.launch(kind, {0, 3, 0}, {0, 3, 40}, 1, nullptr, 2);
        pool.update(0.2f, nullptr, {}, {}, items);
        REQUIRE(pool.count() == 1);
        CHECK(pool.missile(0).position.z > 10);
        pool.saveLocal(state);
    };
    EnemyMissiles::LocalSnapshot forward;
    follow(forward);
    const auto hits = pool.takeHits();
    const auto rockHits = pool.takeRockHits();
    const auto pickupBlasts = pool.takePickupBlasts();
    REQUIRE(rockHits.size() == 2); // pending first impact makes the second pierce
    REQUIRE(pool.restoreLocal(baseline));
    EnemyMissiles::LocalSnapshot current;
    follow(current);
    CHECK(pool.equalLocal(current, forward));
    CHECK(pool.takeHits() == hits);
    CHECK(pool.takeRockHits() == rockHits);
    CHECK(pool.takePickupBlasts() == pickupBlasts);
    CHECK(pool.takeHits().empty());
    CHECK(pool.takeRockHits().empty());
    CHECK(pool.takePickupBlasts().empty());
    REQUIRE(pool.restoreLocal(forward));
    pool.saveLocal(current);
    CHECK(pool.equalLocal(current, forward));
    CHECK(pool.takeHits() == hits);
    CHECK(pool.takeRockHits() == rockHits);
    CHECK(pool.takePickupBlasts() == pickupBlasts);
}

TEST_CASE(
    "enemy missile RNG advances even for rejected launches and clear preserves native counters",
    "[rollback][enemy-missiles-local-rewind]") {
    EnemyMissiles pool{771};
    EnemyMissiles::LocalSnapshot original;
    pool.saveLocal(original);
    EnemyMissileLaunch launch;
    launch.body = {0, 3, 0};
    launch.target = {0, 3, 20};
    launch.facing = kPi;
    launch.aimError = 0.8f;
    CHECK_FALSE(pool.launch(EnemyMissileKind::arrow(), launch));
    // Nothing flies, so the only difference is the random draw.
    CHECK(pool.count() == 0);
    EnemyMissiles::LocalSnapshot backwards;
    pool.saveLocal(backwards);
    CHECK_FALSE(pool.equalLocal(backwards, original));
    launch.facing = 0;
    Obstacle wall;
    wall.centre = {0, 0, 1};
    wall.halfAcross = 2;
    wall.halfAlong = 2;
    wall.height = 20;
    CHECK_FALSE(pool.launch(EnemyMissileKind::arrow(), launch, nullptr, std::array{wall}));
    CHECK(pool.count() == 0);
    EnemyMissiles::LocalSnapshot blocked;
    pool.saveLocal(blocked);
    CHECK_FALSE(pool.equalLocal(blocked, backwards));
    REQUIRE(pool.launch(EnemyMissileKind::arrow(), launch));
    EnemyMissiles::LocalSnapshot forward;
    pool.saveLocal(forward);
    REQUIRE(pool.restoreLocal(blocked));
    REQUIRE(pool.launch(EnemyMissileKind::arrow(), launch));
    EnemyMissiles::LocalSnapshot current;
    pool.saveLocal(current);
    CHECK(pool.equalLocal(current, forward));
    REQUIRE(pool.count() == 1);
    const u64 flight = pool.missile(0).instance;
    pool.blast({.position = {70, 3, 0}, .stages = {1}});
    REQUIRE(pool.bursts().size() == 1);
    const u64 blast = pool.bursts().back().instance;
    CHECK(blast == flight + 1);
    pool.setShrink(0.5f);
    pool.clear();
    CHECK(pool.count() == 0);
    CHECK(pool.shrink() == 0.5f);
    // The next lifetime continues past the blast's.
    pool.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, -1);
    REQUIRE(pool.count() == 1);
    CHECK(pool.missile(0).instance == blast + 1);
}

TEST_CASE("restored enemy lobs retain exact world impacts and liquid collision ownership",
          "[rollback][enemy-missiles-local-rewind]") {
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
    EnemyMissiles::LocalSnapshot baseline;
    pool.saveLocal(baseline);
    REQUIRE(pool.count() == 1);
    std::vector<EnemyMissiles::LocalSnapshot> forward(120);
    for (auto& state : forward) {
        pool.update(step, &collision, {});
        pool.saveLocal(state);
    }
    const auto hits = pool.takeHits();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].worldContact);
    CHECK(hits[0].worldObject == 73);
    CHECK(hits[0].liquid == water);
    REQUIRE(pool.restoreLocal(baseline));
    EnemyMissiles::LocalSnapshot current;
    for (usize tick = 0; tick < forward.size(); ++tick) {
        CAPTURE(water, step, tick);
        pool.update(step, &collision, {});
        pool.saveLocal(current);
        CHECK(pool.equalLocal(current, forward[tick]));
    }
}

TEST_CASE("enemy missile rewind keeps the original loaded models through shooter recycling",
          "[rollback][enemy-missiles-local-rewind][assets]") {
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
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}) == 0);
    EnemyMissiles pool;
    pool.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, arrow, 0);
    EnemyMissiles::LocalSnapshot baseline;
    pool.saveLocal(baseline);
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
    EnemyMissiles::LocalSnapshot forward;
    pool.saveLocal(forward);
    REQUIRE(pool.restoreLocal(baseline));
    CHECK(pool.missile(0).model == arrow);
    CHECK(pool.missile(0).model != otherArrow);
    CHECK(pool.missile(0).shooter == 0);
    pool.update(0.2f, nullptr, {});
    EnemyMissiles::LocalSnapshot current;
    pool.saveLocal(current);
    CHECK(pool.equalLocal(current, forward));
    CHECK(device.texturesCreated == uploads);
    pool.clear(); // before the stock owner releases its resources
}

TEST_CASE("native enemy missile rewind discards wrong flight branches and preserves pending hits",
          "[enemy-missiles-local-rewind]") {
    Battle reference;
    Battle predicted;
    EnemyMissiles::LocalSnapshot saved;
    EnemyMissiles::LocalSnapshot current;
    reference.advance(0.173f);
    predicted.advance(0.173f);
    bool sawBursts = false;
    bool sawReflections = false;
    s32 tick = 0;
    for (s32 window = 0; window < 48; ++window) {
        CAPTURE(window, tick);
        const s32 distance = 1 + window % 8;
        predicted.pool.saveLocal(saved);
        for (s32 step = 0; step < distance; ++step) {
            predicted.step(tick + step + 999, 1.0f / 30);
        }
        predicted.pool.takeHits();
        predicted.pool.takeRockHits();
        predicted.pool.takePickupBlasts();
        const auto callbacks = predicted.callbacks;
        REQUIRE(predicted.pool.restoreLocal(saved));
        CHECK(predicted.callbacks == callbacks);
        predicted.pool.saveLocal(current);
        CHECK(predicted.pool.equalLocal(current, saved));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            reference.step(tick, 1.0f / 60);
            predicted.step(tick, 1.0f / 60);
            CHECK(test::liveDigest(predicted.pool) == test::liveDigest(reference.pool));
            sawBursts |= reference.pool.burstCount() != 0;
            for (usize index = 0; index < reference.pool.count(); ++index) {
                sawReflections |= reference.pool.missile(index).reflected;
            }
        }
        REQUIRE(predicted.pool.canRestoreLocal(saved));
    }
    // The reference never takes its hits, so its queue holds every one it has had.
    CHECK_FALSE(reference.pool.takeHits().empty());
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
    EnemyMissiles::LocalSnapshot untouched;
    other.pool.saveLocal(untouched);
    CHECK_FALSE(other.pool.restoreLocal(saved));
    CHECK_FALSE(other.pool.equalLocal(saved, saved));
    EnemyMissiles::LocalSnapshot current;
    other.pool.saveLocal(current);
    CHECK(other.pool.equalLocal(current, untouched));
    REQUIRE(first.pool.restoreLocal(saved));
    REQUIRE(first.pool.restoreLocal(saved));
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
    REQUIRE(pool.bursts().size() == 1);
    const u64 blast = pool.bursts()[0].instance;
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
    };
    run();
    pool.saveLocal(expectedNative);
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(pool.restoreLocal(saved));
        for (s32 tick = 0; tick < distance; ++tick) {
            pool.blast({.position = {80, 3, 0}, .stages = {1}});
            pool.update(1.0f / 60, nullptr, {});
        }
        REQUIRE(pool.restoreLocal(saved));
        run();
        pool.saveLocal(actualNative);
        CHECK(pool.equalLocal(expectedNative, actualNative));
    }
}

} // namespace
