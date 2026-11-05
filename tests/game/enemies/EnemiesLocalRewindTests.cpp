#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/level/NativeStateResources.h"
#include "game/rollback/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;
constexpr std::array kKinds{kGruntKind, kRatKind, 13, 24, kDeathKind, kItKind, kGarmBroodKind};
constexpr std::string_view kPrefix = "stage/enemies-local-rewind/swarm";

struct Pool {
    test::FakeRenderDevice device;
    Enemies enemies;
    std::array<EnemyView, 2> players;
    std::array<Obstacle, 1> obstacles;

    Pool() {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        enemies.open(device, root, nullptr, Enemies::kMost, {}, 125);
        for (const s32 kind : kKinds) {
            REQUIRE(enemies.loadKind(kind));
        }
        enemies.setContactGridBounds({-100, -10, -100}, {100, 20, 100});
        LookoutRoute route;
        route.points = {{-12, 0, 8}, {8, 0, 12}, {12, 0, -8}};
        route.next = {1, 2, 0};
        enemies.setLookouts(route);
        players[0].player = 0;
        players[0].position = {-6, 0, 0};
        players[1].player = 1;
        players[1].position = {6, 0, 0};
        obstacles[0].centre = {0, 0, 14};
        obstacles[0].halfAcross = 3;
        obstacles[0].halfAlong = 2;
        obstacles[0].height = 6;
        for (s32 slot = 0; slot < Enemies::kMost; ++slot) {
            EnemySpawn spawn;
            spawn.kind = kKinds[static_cast<usize>(slot) % kKinds.size()];
            spawn.tier = slot % 3 + 1;
            spawn.algorithm = slot % 2 == 0 ? kWanderWay : kChaseWay;
            spawn.placed = true;
            spawn.asleep = slot == 3;
            spawn.generator = slot % 4;
            spawn.position = {
                static_cast<f32>(slot % 5 - 2) * 9, 0,
                // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
                static_cast<f32>(slot / 5 - 2) * 9};
            REQUIRE(enemies.spawn(spawn, {}) == slot);
        }
    }

    void step(s32 tick) {
        players[0].position.x = -6 + static_cast<f32>(tick % 12) * 0.125f;
        players[1].position.z = static_cast<f32>(tick % 9) * -0.125f;
        if (tick == 3) {
            enemies.wake(3);
        }
        if (tick == 5 || tick == 24) {
            EnemyHit hit;
            hit.damage = 2;
            hit.player = 0;
            hit.flags = EnemyHit::kKnockBack;
            hit.direction = {1, 0, 0};
            enemies.hurt(0, hit);
        }
        if (tick == 9) {
            EnemyHit hit;
            hit.damage = 1000;
            hit.player = 1;
            hit.flags = EnemyHit::kMagic;
            enemies.hurt(4, hit); // Death's separate transfer/lifecycle queue
        }
        if (tick == 13 || tick == 39) {
            EnemySpawn spawn;
            spawn.kind = kRatKind;
            spawn.generator = 2;
            spawn.placed = true;
            spawn.position = {35, 0, 32};
            spawn.priority = EnemySpawn::Priority::Visible;
            REQUIRE(enemies.spawn(spawn, players, obstacles));
        }
        enemies.setShrink(tick >= 18 && tick < 28 ? 0.5f : 1);
        enemies.update(1, kStep, players, obstacles, nullptr, 1, tick >= 30 && tick < 34);
    }
};

/** The pool's native digest, its artwork named alike in every load. */
NativeStateDigest::Digest digestOf(const Enemies& enemies) {
    NativeStateResources resources;
    REQUIRE(enemies.registerNativeResources(resources, kPrefix));
    return test::liveDigest(enemies, resources.resources());
}

TEST_CASE("a whole native enemy pool rewinds RNG slot order clocks and pending gameplay events",
          "[rollback][enemies][enemies-local-rewind][assets]") {
    Pool pool;
    REQUIRE(pool.enemies.count() == Enemies::kMost);
    pool.enemies.update(1, kStep, pool.players, pool.obstacles);
    // Seventy-two records of the whole pool are too much for the stack: on the heap.
    const auto initial = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*initial));
    CHECK(pool.enemies.takeGeneratorEvents().size() == Enemies::kMost);
    REQUIRE(pool.enemies.restoreLocal(*initial));
    const auto uploads = pool.device.texturesCreated;
    std::vector<Enemies::LocalSnapshot> forward(72);
    for (s32 tick = 0; tick < 72; ++tick) {
        pool.step(tick);
        REQUIRE(pool.enemies.saveLocal(forward[static_cast<usize>(tick)]));
    }
    CHECK_FALSE(pool.enemies.equalLocal(forward.back(), *initial));
    CHECK_FALSE(pool.enemies.takeFeedback().empty());
    CHECK_FALSE(pool.enemies.takeDeathEvents().empty());
    REQUIRE(pool.enemies.restoreLocal(*initial));
    const auto replay = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*replay));
    CHECK(pool.enemies.equalLocal(*replay, *initial));
    for (s32 tick = 0; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        REQUIRE(pool.enemies.saveLocal(*replay));
        CHECK(pool.enemies.equalLocal(*replay, forward[static_cast<usize>(tick)]));
    }
    // Restoring and stepping never load a mesh/texture or dispatch any live-world
    // callback. The queues themselves, not just actor observations, were compared.
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("enemy rewind restores every pending output without dispatching or duplicating it",
          "[rollback][enemies][enemies-local-rewind][assets]") {
    Pool pool;
    pool.enemies.update(1, kStep, pool.players, pool.obstacles);
    for (s32 tick = 0; tick < 72; ++tick) {
        pool.step(tick);
    }
    const auto saved = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*saved));
    const auto take = [&] {
        return std::tuple{pool.enemies.takeBlows(),          pool.enemies.takeLosses(),
                          pool.enemies.takeBursts(),         pool.enemies.takeCues(),
                          pool.enemies.takeFeedback(),       pool.enemies.takeDeathEvents(),
                          pool.enemies.takeDeathShots(),     pool.enemies.takeTagged(),
                          pool.enemies.takeGeneratorEvents()};
    };
    const auto before = take();
    // The native run's own events are restored, so a queue cannot pass by being empty.
    REQUIRE_FALSE(std::get<4>(before).empty());
    REQUIRE_FALSE(std::get<5>(before).empty());
    const auto drained = take();
    CHECK(std::get<4>(drained).empty());
    CHECK(std::get<5>(drained).empty());
    REQUIRE(pool.enemies.restoreLocal(*saved));
    CHECK((take() == before));
    REQUIRE(pool.enemies.restoreLocal(*saved));
    const auto current = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*current));
    CHECK(pool.enemies.equalLocal(*current, *saved));
    CHECK((take() == before));
    CHECK((take() == drained));
}

TEST_CASE("local enemy rewind copies native state across every one to eight tick window",
          "[local-rewind][enemies][native-equality][assets]") {
    Pool pool;
    for (s32 tick = 0; tick < 20; ++tick) {
        pool.step(tick);
    }
    // Two records of the whole pool are too much for the stack: on the heap.
    struct Records {
        std::array<Enemies::LocalSnapshot, 8> history;
        std::array<Enemies::LocalSnapshot, 8> nativeExpected;
    };
    const auto records = std::make_unique<Records>();
    auto& history = records->history;
    auto& nativeExpected = records->nativeExpected;
    for (s32 offset = 0; offset < 8; ++offset) {
        const auto index = static_cast<usize>(offset);
        REQUIRE(pool.enemies.saveLocal(history[index]));
        pool.step(20 + offset);
        REQUIRE(pool.enemies.saveLocal(nativeExpected[index]));
    }
    const auto uploads = pool.device.texturesCreated;
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        // The same saved memory remains reusable, not a one-shot restore token.
        for (s32 repeat = 0; repeat < 2; ++repeat) {
            REQUIRE(pool.enemies.restoreLocal(history[static_cast<usize>(8 - distance)]));
            for (s32 offset = 8 - distance; offset < 8; ++offset) {
                pool.step(20 + offset);
                Enemies::LocalSnapshot corrected;
                REQUIRE(pool.enemies.saveLocal(corrected));
                CHECK(
                    pool.enemies.equalLocal(corrected, nativeExpected[static_cast<usize>(offset)]));
            }
        }
    }
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("local enemy rewind branches input without retaining discarded deaths or rewards",
          "[local-rewind][enemies][assets]") {
    Pool replay;
    Pool reference;
    for (s32 tick = 0; tick < 7; ++tick) {
        replay.step(tick);
        reference.step(tick);
    }
    Enemies::LocalSnapshot saved;
    REQUIRE(replay.enemies.saveLocal(saved));
    const auto births = replay.enemies.takeBirths();
    REQUIRE(replay.enemies.restoreLocal(saved));
    for (s32 tick = 7; tick < 15; ++tick) {
        replay.step(tick);
    }
    const auto wrongFuture = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(replay.enemies.saveLocal(*wrongFuture));
    REQUIRE(replay.enemies.restoreLocal(saved));
    const auto current = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(replay.enemies.saveLocal(*current));
    CHECK(replay.enemies.equalLocal(*current, saved));
    EnemyHit changedInput;
    changedInput.damage = 1000;
    changedInput.player = 0;
    changedInput.flags = EnemyHit::kMagic;
    replay.enemies.hurt(0, changedInput);
    reference.enemies.hurt(0, changedInput);
    for (s32 tick = 7; tick < 15; ++tick) {
        replay.step(tick);
        reference.step(tick);
        CHECK(digestOf(replay.enemies) == digestOf(reference.enemies));
    }
    REQUIRE(replay.enemies.saveLocal(*current));
    CHECK_FALSE(replay.enemies.equalLocal(*current, *wrongFuture));
    // Consuming an output queue in a discarded future cannot consume its saved past.
    REQUIRE(replay.enemies.restoreLocal(saved));
    CHECK(replay.enemies.takeBirths() == births);
    REQUIRE(replay.enemies.restoreLocal(saved));
    CHECK(replay.enemies.takeBirths() == births);
}

TEST_CASE("local enemy rewind retains lazy artwork but rewinds its active roster and clocks",
          "[local-rewind][enemies][assets]") {
    Pool pool;
    const auto draw = [&] {
        pool.device.draws.clear();
        pool.enemies.draw(pool.device, Mat4{1}, {}, nullptr, nullptr, nullptr, 0.5f);
        return pool.device.draws;
    };
    const auto sameDrawing = [](const auto& left, const auto& right) {
        return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
            return a.texture == b.texture && a.vertices == b.vertices &&
                   a.transform == b.transform && a.state == b.state;
        });
    };
    const auto originalDrawing = draw();
    REQUIRE_FALSE(originalDrawing.empty());
    constexpr s32 kNewKind = 0;
    REQUIRE_FALSE(pool.enemies.kindLoaded(kNewKind));
    Enemies::LocalSnapshot saved;
    REQUIRE(pool.enemies.saveLocal(saved));
    REQUIRE(pool.enemies.loadKind(kNewKind));
    EnemySpawn spawn;
    spawn.kind = kNewKind;
    spawn.placed = true;
    spawn.priority = EnemySpawn::Priority::Visible;
    spawn.position = {80, 0, 80};
    const auto slot = pool.enemies.spawn(spawn, {});
    REQUIRE(slot);
    for (s32 tick = 0; tick < 8; ++tick) {
        pool.step(tick);
    }
    const auto expected = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*expected));
    const auto futureDrawing = draw();
    REQUIRE_FALSE(futureDrawing.empty());
    REQUIRE_FALSE(sameDrawing(originalDrawing, futureDrawing));
    const auto uploads = pool.device.texturesCreated;
    REQUIRE(pool.enemies.restoreLocal(saved));
    CHECK_FALSE(pool.enemies.kindLoaded(kNewKind));
    const auto current = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.enemies.saveLocal(*current));
    CHECK(pool.enemies.equalLocal(*current, saved));
    CHECK(sameDrawing(draw(), originalDrawing));
    REQUIRE(pool.enemies.loadKind(kNewKind));
    CHECK(pool.enemies.spawn(spawn, {}) == slot);
    for (s32 tick = 0; tick < 8; ++tick) {
        pool.step(tick);
    }
    REQUIRE(pool.enemies.saveLocal(*current));
    CHECK(pool.enemies.equalLocal(*current, *expected));
    CHECK(sameDrawing(draw(), futureDrawing));
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("local enemy snapshots reject closed foreign and replacement lifetimes",
          "[local-rewind][enemies][native-equality]") {
    test::FakeRenderDevice device;
    auto storage = std::make_unique<std::optional<Enemies>>(std::in_place);
    auto& pool = **storage;
    Enemies foreign;
    Enemies::LocalSnapshot saved;
    CHECK_FALSE(pool.saveLocal(saved));
    CHECK_FALSE(pool.equalLocal(saved, saved));
    pool.open(device, {}, nullptr, Enemies::kMost, {}, 125);
    CHECK_FALSE(pool.restoreLocal(saved));
    REQUIRE(pool.saveLocal(saved));
    CHECK(pool.equalLocal(saved, saved));
    REQUIRE(pool.restoreLocal(saved));
    foreign.open(device, {}, nullptr, Enemies::kMost, {}, 125);
    CHECK_FALSE(foreign.restoreLocal(saved));
    CHECK_FALSE(foreign.equalLocal(saved, saved));
    pool.close();
    CHECK_FALSE(pool.restoreLocal(saved));
    CHECK_FALSE(pool.equalLocal(saved, saved));
    pool.open(device, {}, nullptr, Enemies::kMost, {}, 126);
    CHECK_FALSE(pool.restoreLocal(saved));
    const auto* address = &pool;
    REQUIRE(pool.saveLocal(saved));
    storage->reset();
    auto& replacement = storage->emplace();
    CHECK(&replacement == address);
    replacement.open(device, {}, nullptr, Enemies::kMost, {}, 127);
    CHECK_FALSE(replacement.restoreLocal(saved));
    CHECK_FALSE(replacement.equalLocal(saved, saved));
}

TEST_CASE("native enemy equality sees pending work even when no body is visible",
          "[local-rewind][enemies][native-equality]") {
    test::FakeRenderDevice device;
    Enemies pool;
    pool.open(device, {}, nullptr, Enemies::kMost, {}, 125);
    REQUIRE(pool.loadKind(kItKind));
    Enemies::LocalSnapshot initial;
    REQUIRE(pool.saveLocal(initial));
    const auto changed = [&](auto&& change) {
        REQUIRE(pool.restoreLocal(initial));
        change();
        Enemies::LocalSnapshot after;
        REQUIRE(pool.saveLocal(after));
        CHECK_FALSE(pool.equalLocal(initial, after));
        CHECK_FALSE(pool.equalLocal(after, initial));
        REQUIRE(pool.restoreLocal(initial));
        REQUIRE(pool.saveLocal(after));
        CHECK(pool.equalLocal(initial, after));
    };
    changed([&] { pool.update(1, 0, {}); });         // the two-tick AI has one unspent tick
    changed([&] { pool.update(0, 1.0f / 60, {}); }); // texture clocks without any actor
    changed([&] { pool.setContactGridBounds({-20, -10, -30}, {40, 10, 50}); });
    changed([&] { pool.setShrink(0.5f); });
    changed([&] { pool.setView(ViewVolume{}); });
    changed([&] { pool.setLookouts({{{1, 2, 3}, {4, 5, 6}}, {1, 0}}); });

    // A native birth advances the RNG and monotonic instance source, and emits
    // pending outputs even for IT's zero-node art. Consuming those outputs is
    // itself native state, rather than an ignorable presentation observation.
    REQUIRE(pool.spawn({.kind = kItKind, .position = {10, 0, 0}, .placed = true}, {}));
    Enemies::LocalSnapshot born;
    Enemies::LocalSnapshot consumed;
    REQUIRE(pool.saveLocal(born));
    CHECK_FALSE(pool.equalLocal(initial, born));
    REQUIRE_FALSE(pool.takeBirths().empty());
    REQUIRE(pool.saveLocal(consumed));
    CHECK_FALSE(pool.equalLocal(born, consumed));
    REQUIRE(pool.restoreLocal(born));
    REQUIRE(pool.saveLocal(consumed));
    CHECK(pool.equalLocal(born, consumed));
}

TEST_CASE("local rewind of the native bodiless enemy runs without private game assets",
          "[local-rewind][enemies]") {
    test::FakeRenderDevice device;
    Enemies pool;
    pool.open(device, {}, nullptr, Enemies::kMost, {}, 125);
    REQUIRE(pool.loadKind(kItKind));
    EnemySpawn spawn;
    spawn.kind = kItKind;
    spawn.placed = true;
    spawn.algorithm = kChaseWay;
    spawn.position = {10, 0, 0};
    REQUIRE(pool.spawn(spawn, {}));
    std::array<EnemyView, 1> players;
    players[0].player = 0;
    for (s32 tick = 0; tick < 7; ++tick) {
        pool.update(1, kStep, players);
    }
    Enemies::LocalSnapshot saved;
    REQUIRE(pool.saveLocal(saved));
    // Eight records of the whole pool are too much for the stack: on the heap.
    const auto future = std::make_unique<std::array<Enemies::LocalSnapshot, 8>>();
    for (auto& state : *future) {
        pool.update(1, kStep, players);
        REQUIRE(pool.saveLocal(state));
    }
    auto moved = std::move(saved);
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from value is deliberately exercised
    CHECK_FALSE(pool.restoreLocal(saved));
    REQUIRE(pool.restoreLocal(moved));
    const auto current = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(pool.saveLocal(*current));
    CHECK(pool.equalLocal(*current, moved));
    for (const auto& state : *future) {
        pool.update(1, kStep, players);
        REQUIRE(pool.saveLocal(*current));
        CHECK(pool.equalLocal(*current, state));
    }
    REQUIRE(pool.restoreLocal(moved));
    REQUIRE(pool.saveLocal(*current));
    CHECK(pool.equalLocal(*current, moved));
    const HazardSurfaces replacementHazards;
    pool.setHazards(&replacementHazards);
    CHECK_FALSE(pool.restoreLocal(moved));
    pool.setHazards(nullptr);
    REQUIRE(pool.restoreLocal(moved));
}

TEST_CASE("measure crowded native enemy save restore and eight tick replay costs",
          "[.][local-rewind-timing][assets]") {
    Pool pool;
    for (s32 tick = 0; tick < 20; ++tick) {
        pool.step(tick);
    }
    Enemies::LocalSnapshot saved;
    using Clock = std::chrono::steady_clock;
    const auto coldStart = Clock::now();
    const bool coldSaved = pool.enemies.saveLocal(saved);
    const auto coldEnd = Clock::now();
    REQUIRE(coldSaved);
    std::vector<f64> saves;
    std::vector<f64> restores;
    std::vector<f64> replays;
    saves.reserve(256);
    restores.reserve(256);
    replays.reserve(256);
    for (s32 sample = 0; sample < 256; ++sample) {
        const auto saveStart = Clock::now();
        const bool copied = pool.enemies.saveLocal(saved);
        const auto savedAt = Clock::now();
        REQUIRE(copied);
        for (s32 tick = 20; tick < 28; ++tick) {
            pool.step(tick);
        }
        const auto restoreStart = Clock::now();
        const bool restored = pool.enemies.restoreLocal(saved);
        const auto restoredAt = Clock::now();
        REQUIRE(restored);
        const auto replayStart = Clock::now();
        for (s32 tick = 20; tick < 28; ++tick) {
            pool.step(tick);
        }
        const auto replayedAt = Clock::now();
        saves.push_back(std::chrono::duration<f64, std::milli>(savedAt - saveStart).count());
        restores.push_back(
            std::chrono::duration<f64, std::milli>(restoredAt - restoreStart).count());
        replays.push_back(std::chrono::duration<f64, std::milli>(replayedAt - replayStart).count());
        REQUIRE(pool.enemies.restoreLocal(saved));
    }
    const auto report = [](const char* operation, const std::vector<f64>& values) {
        const auto summary = test::summarizeNativeTimings(values);
        REQUIRE(summary);
        std::cout << "enemy-local-" << operation << " samples=" << summary->samples
                  << " mean_ms=" << summary->mean << " p95_ms=" << summary->p95
                  << " p99_ms=" << summary->p99 << " max_ms=" << summary->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
    std::cout << "enemy-local-cold-save ms="
              << std::chrono::duration<f64, std::milli>(coldEnd - coldStart).count() << '\n';
    std::cout << "enemy-local-state inline_bytes=" << sizeof(Enemies::LocalSnapshot)
              << " (excludes dynamic buffers; enemy pool only, not a scene budget)\n";
}

TEST_CASE("native hits and bomber bursts retain the emitting occupant after its slot is reused",
          "[enemies][local-rewind][native-combat-audio][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {.health = 20}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    REQUIRE(enemies.spawn(
                {.kind = kGruntKind, .tier = kSuicideStrength, .algorithm = 0, .placed = true},
                {}) == 0);
    REQUIRE(enemies.algorithmOf(0) == kSuicideWay);
    const u64 first = enemies.observe(0)->instance;
    Enemies::LocalSnapshot saved;
    REQUIRE(enemies.saveLocal(saved));
    struct Outcome {
        std::unique_ptr<Enemies::LocalSnapshot> state = std::make_unique<Enemies::LocalSnapshot>();
        std::vector<EnemyFeedback> feedback;
        std::vector<EnemyBurst> bursts;
    };
    const auto hitAndReplace = [&] {
        enemies.hurt(0, {.damage = 1, .player = 0});
        enemies.hurt(0, {.damage = 100000, .player = 0});
        REQUIRE(enemies.spawn({.kind = kGruntKind,
                               .position = {5, 0, 0},
                               .placed = true,
                               .priority = EnemySpawn::Priority::Visible},
                              {}) == 0);
        CHECK(enemies.observe(0)->instance != first);
        Outcome outcome;
        REQUIRE(enemies.saveLocal(*outcome.state));
        outcome.feedback = enemies.takeFeedback();
        outcome.bursts = enemies.takeBursts();
        REQUIRE(outcome.feedback.size() == 2);
        REQUIRE(outcome.bursts.size() == 1);
        for (const auto& event : outcome.feedback) {
            CHECK(event.enemy == 0);
            CHECK(event.instance == first);
        }
        CHECK(outcome.bursts[0].instance == first);
        CHECK(outcome.bursts[0].enemy == 0);
        return outcome;
    };
    const auto expected = hitAndReplace();
    for (s32 ticks = 1; ticks <= 8; ++ticks) {
        CAPTURE(ticks);
        REQUIRE(enemies.restoreLocal(saved));
        for (s32 tick = 0; tick < ticks; ++tick) {
            enemies.update(1, kStep, {});
            enemies.hurt(0, {.damage = 2, .player = 1});
        }
        REQUIRE(enemies.restoreLocal(saved));
        const auto actual = hitAndReplace();
        CHECK(enemies.equalLocal(*actual.state, *expected.state));
        CHECK(actual.feedback == expected.feedback);
        CHECK(actual.bursts == expected.bursts);
    }
}
} // namespace
