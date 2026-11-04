#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/critters/Critters.h"
#include "game/level/NativeStateResources.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The native digest of `critters`, its families registered under the keys every load of
 * them shares, so two populations compare alike. */
NativeStateDigest::Digest digestOf(const Critters& critters) {
    NativeStateResources resources;
    REQUIRE(critters.registerNativeResources(resources, "critters"));
    return test::liveDigest(critters, resources.resources());
}

struct NativePopulation {
    test::FakeRenderDevice device;
    Critters critters;
    NativePopulation() {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        critters.open(device, root, nullptr, {}, 'G');
        critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
    }
    NativeStateDigest::Digest digest() const { return digestOf(critters); }
    void step(s32 tick, bool incorrect = false) {
        if (tick % 24 == 0) {
            const s32 kind = (tick / 24 + (incorrect ? 1 : 0)) % 3;
            const Vec3 position{static_cast<f32>(tick % 19 - 9), 0, static_cast<f32>(tick % 23)};
            if (kind == 0) {
                critters.spawnGeneral(position, 0, 20);
            }
            if (kind == 1) {
                critters.spawnGolem(position, 0);
            }
            if (kind == 2) {
                critters.spawnGargoyle(position, 0);
            }
        }
        if (tick % 17 == 0) {
            EnemyHit hit;
            hit.player = 3;
            hit.damage = incorrect ? 1.0f : 100000.0f;
            hit.level = 99;
            critters.hurt((tick / 17) % Critters::kMost, hit);
        }
        if (tick % 37 == 0) {
            critters.freeze(0, 5);
        }
        if (tick % 43 == 0) {
            critters.blind(1, 7);
        }
        std::array<EnemyView, 4> players;
        for (s32 seat = 0; seat < 4; ++seat) {
            auto& player = players[static_cast<usize>(seat)];
            player.player = seat;
            player.radius = 1;
            player.height = 6;
            player.position = {static_cast<f32>(seat * 4 - 7), 0,
                               static_cast<f32>(tick % 21 + (incorrect ? 30 : 3))};
        }
        critters.update(1, 1.0f / 60, players, tick % 61 < 5);
        if (tick % 13 == 0) {
            critters.takeBlows();
            critters.takeCues();
            critters.takeShots();
            critters.takeLosses();
        }
    }
};

TEST_CASE("native great-creature population rewinds spawns queues patrols and bank clocks",
          "[local-rewind][critters-local-rewind][native-equality][assets]") {
    auto reference = std::make_unique<NativePopulation>();
    auto predicted = std::make_unique<NativePopulation>();
    LocalStateHistory<Critters::LocalSnapshot> history;
    s32 tick = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        const s32 distance = window % 8 + 1;
        REQUIRE(history.capture(static_cast<u64>(tick),
                                [&](auto& state) { return predicted->critters.saveLocal(state); }));
        const auto* saved = history.find(static_cast<u64>(tick));
        REQUIRE(saved);
        std::vector<Critters::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            predicted->step(tick + i);
            REQUIRE(predicted->critters.saveLocal(nativeExpected[static_cast<usize>(i)]));
        }
        REQUIRE(predicted->critters.restoreLocal(*saved));
        for (s32 i = 0; i < distance; ++i) {
            predicted->step(tick + i, true);
        }
        REQUIRE(predicted->critters.restoreLocal(*saved));
        CHECK(predicted->digest() == reference->digest());
        for (s32 i = 0; i < distance; ++i) {
            CAPTURE(i);
            reference->step(tick + i);
            predicted->step(tick + i);
            CHECK(predicted->digest() == reference->digest());
            auto corrected = std::make_unique<Critters::LocalSnapshot>();
            REQUIRE(predicted->critters.saveLocal(*corrected));
            CHECK(
                predicted->critters.equalLocal(*corrected, nativeExpected[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(predicted->device.textureUpdates == 0);
}

TEST_CASE("native population retains speculative artwork without retaining its active catalog",
          "[local-rewind][critters-local-rewind][native-equality][assets]") {
    auto population = std::make_unique<NativePopulation>();
    auto& critters = population->critters;
    Critters::LocalSnapshot cold;
    Critters::LocalSnapshot general;
    REQUIRE(critters.saveLocal(cold));
    const auto empty = population->digest();
    REQUIRE(critters.spawnGeneral({}, 0, 15) == 0);
    REQUIRE(critters.saveLocal(general));
    const auto baseline = population->digest();
    const auto* artwork = critters.resources().front();
    const auto* actor = &critters.fighters().front();
    REQUIRE(critters.spawnGolem({4, 0, 3}, 0) == 1);
    REQUIRE(critters.spawnGargoyle({8, 0, 3}, 0) == 2);
    const auto uploads = population->device.texturesCreated;
    REQUIRE(critters.restoreLocal(cold));
    CHECK(population->digest() == empty);
    CHECK(critters.resources().empty());
    REQUIRE(critters.spawnGeneral({}, 0, 15) == 0);
    CHECK(critters.resources().front() == artwork);
    CHECK(&critters.fighters().front() == actor);
    CHECK(population->digest() == baseline);
    REQUIRE(critters.spawnGolem({4, 0, 3}, 0) == 1);
    REQUIRE(critters.spawnGargoyle({8, 0, 3}, 0) == 2);
    REQUIRE(critters.restoreLocal(general));
    CHECK(population->digest() == baseline);
    CHECK(population->device.texturesCreated == uploads);
    critters.setLookouts({{{20, 0, 0}, {20, 0, 20}}, {1, 65535}});
    REQUIRE(critters.restoreLocal(general));
    CHECK(population->digest() == baseline);
    auto corrected = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(critters.saveLocal(*corrected));
    CHECK(critters.equalLocal(general, *corrected));
}

TEST_CASE("native population snapshots cannot cross ownership or stage unload",
          "[local-rewind][critters-local-rewind][native-equality]") {
    auto first = std::make_unique<Critters>();
    auto other = std::make_unique<Critters>();
    auto saved = std::make_unique<Critters::LocalSnapshot>();
    CHECK_FALSE(first->equalLocal(*saved, *saved));
    REQUIRE(first->saveLocal(*saved));
    CHECK(first->equalLocal(*saved, *saved));
    CHECK_FALSE(other->restoreLocal(*saved));
    CHECK_FALSE(other->equalLocal(*saved, *saved));
    REQUIRE(first->restoreLocal(*saved));
    first->close();
    CHECK_FALSE(first->restoreLocal(*saved));
    CHECK_FALSE(first->equalLocal(*saved, *saved));
    REQUIRE(first->saveLocal(*saved));
    REQUIRE(first->restoreLocal(*saved));
}

TEST_CASE("native empty population equality still tracks its patrol route",
          "[local-rewind][critters-local-rewind][native-equality]") {
    auto population = std::make_unique<Critters>();
    auto initial = std::make_unique<Critters::LocalSnapshot>();
    auto after = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(population->saveLocal(*initial));
    population->setLookouts({{{1, 2, 3}, {4, 5, 6}}, {1, 65535}});
    REQUIRE(population->saveLocal(*after));
    CHECK_FALSE(population->equalLocal(*initial, *after));
    REQUIRE(population->restoreLocal(*initial));
    REQUIRE(population->saveLocal(*after));
    CHECK(population->equalLocal(*initial, *after));
}

/** A population with its golem, general and gargoyle families loaded before anything spawns. */
struct PreloadedPopulation {
    test::FakeRenderDevice device;
    Critters critters;
    PreloadedPopulation() {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        critters.open(device, root, nullptr, {}, 'G');
        critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
        const std::array kinds{CombatantKind::Golem, CombatantKind::General,
                               CombatantKind::Gargoyle};
        for (const auto kind : kinds) {
            REQUIRE(critters.archiveFor(kind) != nullptr);
        }
    }
    void spawn() {
        REQUIRE(critters.spawnGolem({0, 0, 0}, 0) == 0);
        REQUIRE(critters.spawnGeneral({-20, 0, 0}, 0, 4) == 1);
        REQUIRE(critters.spawnGargoyle({20, 0, 0}, 0) == 2);
    }
    NativeStateDigest::Digest digest() const { return digestOf(critters); }
};

std::array<EnemyView, 2> views() {
    std::array<EnemyView, 2> result;
    result[0].player = 3;
    result[0].position = {0, 0, 10};
    result[0].radius = 1;
    result[0].height = 6;
    result[1].player = 1;
    result[1].position = {20, 0, 15};
    result[1].radius = 1;
    result[1].height = 6;
    return result;
}

TEST_CASE("native mixed critter pools resume every slot queue and shared texture bank",
          "[critters-local-rewind][assets]") {
    const s32 rate = GENERATE(30, 60);
    auto first = std::make_unique<PreloadedPopulation>();
    auto replay = std::make_unique<PreloadedPopulation>();
    auto players = views();
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 11;
    hit.flags = 0x10;
    hit.level = 30;
    hit.close = true;
    for (auto* population : {first.get(), replay.get()}) {
        population->spawn();
        for (s32 frame = 0; frame < 30; ++frame) {
            population->critters.update(60 / rate, 1.0f / static_cast<f32>(rate), players);
        }
        population->critters.hurt(0, hit);
        population->critters.blind(2, 7);
        population->critters.freeze(0, 3);
    }
    auto held = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(first->critters.saveLocal(*held));
    REQUIRE_FALSE(first->critters.takeLosses().empty());
    REQUIRE(first->critters.restoreLocal(*held));
    CHECK(replay->digest() == first->digest());
    REQUIRE(replay->critters.count() == 3);
    const u32 uploads = replay->device.texturesCreated;
    for (s32 frame = 0; frame < 120; ++frame) {
        CAPTURE(rate, frame);
        players[0].position.x = frame > 40 ? 4.0f : 0.0f;
        const bool stop = frame >= 25 && frame < 40;
        const s32 ticks = frame == 45 ? 0 : 60 / rate;
        first->critters.update(ticks, 1.0f / static_cast<f32>(rate), players, stop);
        replay->critters.update(ticks, 1.0f / static_cast<f32>(rate), players, stop);
        CHECK(replay->digest() == first->digest());
    }
    CHECK(replay->device.texturesCreated == uploads);
    CHECK(replay->device.textureUpdates == 0);
    CHECK(replay->device.draws.empty());
}

TEST_CASE("critter pool rewind covers newly spawned slots damage queues and bank clocks together",
          "[critters-local-rewind][assets]") {
    auto fixture = std::make_unique<PreloadedPopulation>();
    fixture->spawn();
    auto before = std::make_unique<Critters::LocalSnapshot>();
    auto after = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(fixture->critters.saveLocal(*before));
    REQUIRE(fixture->critters.spawnGeneral({-5, 0, 3}, 0) == 3);
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 12;
    hit.level = 30;
    hit.close = true;
    fixture->critters.hurt(0, hit);
    fixture->critters.update(2, 1.0f / 30, views());
    REQUIRE(fixture->critters.saveLocal(*after));
    REQUIRE_FALSE(fixture->critters.equalLocal(*after, *before));
    REQUIRE(fixture->critters.canRestoreLocal(*before));
    REQUIRE(fixture->critters.restoreLocal(*before));
    REQUIRE(fixture->critters.saveLocal(*after));
    CHECK(fixture->critters.equalLocal(*after, *before));
    CHECK(fixture->critters.count() == 3);
    CHECK(fixture->critters.takeLosses().empty());
}
} // namespace
