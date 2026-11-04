#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/PlacedEnemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlacedEnemies::Placement placement(const Vec3& position, s32 kind = kGruntKind) {
    PlacedEnemies::Placement value;
    value.kind = kind;
    value.spawn.kind = kind;
    value.spawn.position = position;
    value.spawn.placed = true;
    value.spawn.priority = EnemySpawn::Priority::Visible;
    value.viewRadius = 1;
    return value;
}

// The original LevelOpponents phase, independent of the extracted owner.
template <class Stand>
void originalPhase(std::vector<PlacedEnemies::Placement>& pending,
                   const std::optional<ViewVolume>& view, const Vec3& attention, Stand stand) {
    std::erase_if(pending, [&](const auto& record) {
        if (view && (!view->sees(record.spawn.position, record.viewRadius) ||
                     glm::distance(attention, record.spawn.position) > 50.0f)) {
            return false;
        }
        auto standing = record;
        if (!view) {
            standing.spawn.priority = EnemySpawn::Priority::FreeSlotOnly;
        }
        stand(standing);
        return true;
    });
}

std::vector<PlacedEnemies::Placement> pendingRecords(const PlacedEnemies& owner) {
    std::vector<PlacedEnemies::Placement> result;
    for (const usize index : owner.pending()) {
        result.push_back(owner.roster()[index]);
    }
    return result;
}

/** The roster indices still waiting, in native order. */
std::vector<usize> pendingIndices(const PlacedEnemies& owner) {
    return std::vector<usize>(owner.pending().begin(), owner.pending().end());
}

std::vector<PlacedEnemies::Placement> rosterOf(const PlacedEnemies& owner) {
    return std::vector<PlacedEnemies::Placement>(owner.roster().begin(), owner.roster().end());
}

/** Whether the owner's values now equal `saved`. */
bool holds(const PlacedEnemies& owner, const PlacedEnemies::LocalSnapshot& saved) {
    PlacedEnemies::LocalSnapshot now;
    owner.saveLocal(now);
    return owner.equalLocal(now, saved);
}

TEST_CASE("placed enemy checkpoint retains complete metadata and ordered native activation",
          "[rollback][placed-enemies]") {
    auto varied = placement({0, 0, 10});
    varied.spawn.zigZagSide = -1;
    varied.spawn.tier = 4;
    varied.spawn.algorithm = kThrowWay;
    varied.spawn.direction = {0.25f, 0, 0.75f};
    varied.spawn.clearance = 2;
    varied.spawn.generator = 7;
    varied.spawn.patrolBirth = true;
    varied.spawn.allBirthDirections = true;
    varied.spawn.asleep = true;
    varied.spawn.throwInterval = 0.75f;
    varied.spawn.sight = 19.5f;
    varied.facing = 0.625f;
    varied.viewRadius = 2.5f;
    varied.sight = -3;
    varied.carried = 37;
    const std::vector roster{placement({100, 0, 10}), varied, placement({0, 0, 51}), varied,
                             placement({0, 0, 50})};
    PlacedEnemies owner;
    owner.bind(roster);
    const ViewVolume view;
    owner.observe(view, {});
    PlacedEnemies::LocalSnapshot checkpoint;
    owner.saveLocal(checkpoint);
    CHECK(rosterOf(owner) == roster);
    CHECK(pendingIndices(owner) == std::vector<usize>{0, 1, 2, 3, 4});
    CHECK(owner.inView({0, 0, 50}, 1));
    CHECK_FALSE(owner.inView({0, 0, 50.01f}, 1));
    CHECK_FALSE(owner.inView({100, 0, 10}, 1));
    const auto expected = owner.activations();
    CHECK(holds(owner, checkpoint));
    REQUIRE(expected.size() == 3);
    CHECK(expected[0] == varied);
    CHECK(expected[1] == varied); // Identical records are distinct original placements.
    auto native = roster;
    std::vector<PlacedEnemies::Placement> oracle;
    originalPhase(native, view, {}, [&](const auto& standing) { oracle.push_back(standing); });
    std::vector<PlacedEnemies::Placement> actual;
    owner.activate([&](const auto& standing) { actual.push_back(standing); });
    CHECK(actual == oracle);
    CHECK(actual == expected);
    CHECK(pendingRecords(owner) == native);
    CHECK(owner.pending().size() == 2);
    PlacedEnemies::LocalSnapshot after;
    owner.saveLocal(after);
    CHECK(pendingIndices(owner) == std::vector<usize>{0, 2});
    REQUIRE(owner.restoreLocal(checkpoint));
    actual.clear();
    owner.activate([&](const auto& standing) { actual.push_back(standing); });
    CHECK(actual == expected);
    CHECK(holds(owner, after));
}

TEST_CASE("placed enemy no-view activation consumes failed attempts in native order",
          "[rollback][placed-enemies]") {
    const std::vector roster{placement({0, 0, -500}), placement({100, 0, 10}),
                             placement({0, 0, 500})};
    PlacedEnemies owner;
    owner.bind(roster);
    owner.observe(std::nullopt, {123, 456, 789});
    PlacedEnemies::LocalSnapshot before;
    owner.saveLocal(before);
    const auto preflight = owner.activations();
    std::vector<PlacedEnemies::Placement> actual;
    // An empty-result dispatcher represents a full native pool. Activation is
    // still consumed, rather than retried on every later camera update.
    owner.activate([&](const auto& standing) {
        CHECK(standing.spawn.priority == EnemySpawn::Priority::FreeSlotOnly);
        actual.push_back(standing);
    });
    REQUIRE(actual.size() == roster.size());
    CHECK(preflight == actual);
    CHECK(owner.count() == 0);
    owner.activate([&](const auto&) { FAIL("a consumed placement was retried"); });
    REQUIRE(owner.restoreLocal(before));
    CHECK(owner.count() == roster.size());
    CHECK(owner.inView({100000, 0, -100000}, 1));
}

TEST_CASE("placed enemy checkpoint preserves negative authored default-way selectors",
          "[rollback][placed-enemies]") {
    auto record = placement({0, 0, 10});
    record.spawn.algorithm = -32768;
    PlacedEnemies owner;
    owner.bind({record});
    PlacedEnemies::LocalSnapshot state;
    owner.saveLocal(state);
    REQUIRE(owner.restoreLocal(state));
    CHECK(owner.activations().front().spawn.algorithm == -32768);
}

TEST_CASE("placement value preflight matches prospective native watch without mutation",
          "[rollback][placed-enemies]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 10}), placement({0, 0, 100}, kDeathKind),
                placement({0, 0, 100}, kDeathKind)});
    owner.observe(ViewVolume{}, {});
    PlacedEnemies::LocalSnapshot before;
    owner.saveLocal(before);
    const auto current = owner.activations();
    ViewVolume next;
    next.position = {0, 0, 80};
    const Vec3 attention{0, 0, 100};
    owner.observe(next, attention);
    const auto prospective = owner.activations();
    REQUIRE(owner.restoreLocal(before));
    REQUIRE(prospective.size() == 2);
    CHECK(holds(owner, before));
    owner.observe(next, attention);
    CHECK(owner.activations() == prospective);
    std::vector<PlacedEnemies::Placement> born;
    owner.activate([&](const auto& value) { born.push_back(value); });
    CHECK(born == prospective);
    CHECK(current.size() == 1);
}

TEST_CASE("placed enemy native rewind survives watch and activation without repeating callbacks",
          "[placed-enemies-local-rewind]") {
    PlacedEnemies owner;
    PlacedEnemies other;
    const std::vector roster{placement({0, 0, 10}), placement({0, 0, 100})};
    owner.bind(roster);
    other.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    owner.saveLocal(saved);
    CHECK_FALSE(other.restoreLocal(saved));
    std::vector<PlacedEnemies::Placement> calls;
    owner.observe(ViewVolume{}, {1, 0, 0});
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 1);
    owner.observe(std::nullopt, {2, 0, 0});
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 2);
    owner.activate([&](const auto&) { FAIL("consumed placement must remain consumed"); });
    REQUIRE(owner.count() == 0);
    REQUIRE(owner.restoreLocal(saved));
    CHECK(calls.size() == 2);
    REQUIRE(owner.restoreLocal(saved)); // Native snapshots are reusable value history.
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].spawn.position == roster[0].spawn.position);
    CHECK(calls[3].spawn.position == roster[1].spawn.position);
}

TEST_CASE("placed enemy native rewind retains resource lifetime and replacement fences",
          "[placed-enemies-local-rewind]") {
    const s32 mutation = GENERATE(0, 1, 2);
    const std::vector roster{placement({0, 0, 10})};
    auto storage = std::make_unique<std::optional<PlacedEnemies>>();
    auto& owner = storage->emplace();
    owner.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    owner.saveLocal(saved);
    switch (mutation) {
    case 0: owner.bind(roster); break;
    case 1: owner.clear(); break;
    case 2:
        storage->reset();
        storage->emplace().bind(roster);
        break;
    default: break;
    }
    PlacedEnemies::LocalSnapshot after;
    storage->value().saveLocal(after);
    CHECK_FALSE(storage->value().canRestoreLocal(saved));
    CHECK_FALSE(storage->value().restoreLocal(saved));
    CHECK(holds(storage->value(), after));
}

TEST_CASE("placed native births preserve pool replacement failure queues and RNG after correction",
          "[rollback][placed-enemies][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 917);
    REQUIRE(enemies.loadKind(kGruntKind));
    auto existing = placement({0, 0, 20});
    existing.spawn.generator = 4;
    REQUIRE(enemies.spawn(existing.spawn, {}));
    ViewVolume view;
    enemies.setView(view);
    const auto baseline = enemies.checkpoint();
    REQUIRE(baseline);
    const auto uploads = device.texturesCreated;
    auto first = placement({0, 0, 10});
    first.spawn.algorithm = kWanderWay;
    auto second = placement({0, 0, 15});
    second.spawn.algorithm = kZigZagWay;
    const std::vector roster{first, second, placement({0, 0, 100})};
    PlacedEnemies owner;
    owner.bind(roster);
    const bool watched = GENERATE(false, true);
    const auto observation = watched ? std::optional<ViewVolume>{view} : std::nullopt;
    owner.observe(observation, {});
    PlacedEnemies::LocalSnapshot placementsBefore;
    owner.saveLocal(placementsBefore);
    auto native = roster;
    std::vector<std::optional<s32>> expectedSlots;
    originalPhase(native, observation, {}, [&](const auto& record) {
        expectedSlots.push_back(enemies.spawn(record.spawn, {}));
    });
    const auto expected = enemies.checkpoint();
    REQUIRE(expected);
    if (watched) {
        REQUIRE(expectedSlots.size() == 2);
        CHECK(expectedSlots[0].has_value());
        CHECK(expectedSlots[1].has_value());
        CHECK(expected->nextInstance > baseline->nextInstance);
        CHECK(expected->random != baseline->random);
        CHECK(expected->generatorEvents.size() > baseline->generatorEvents.size());
    } else {
        REQUIRE(expectedSlots.size() == 3);
        CHECK(
            std::ranges::none_of(expectedSlots, [](const auto& slot) { return slot.has_value(); }));
        CHECK(expected == baseline);
    }
    REQUIRE(enemies.restore(*baseline));
    std::vector<std::optional<s32>> actualSlots;
    owner.activate(
        [&](const auto& record) { actualSlots.push_back(enemies.spawn(record.spawn, {})); });
    CHECK(actualSlots == expectedSlots);
    CHECK(enemies.checkpoint() == expected);
    CHECK(pendingRecords(owner) == native);
    PlacedEnemies::LocalSnapshot placementsAfter;
    owner.saveLocal(placementsAfter);
    // Delayed correction restores both owners; placement restore itself never
    // spawns or consumes the native random engine.
    REQUIRE(enemies.restore(*baseline));
    REQUIRE(owner.restoreLocal(placementsBefore));
    CHECK(enemies.checkpoint() == baseline);
    owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
    CHECK(enemies.checkpoint() == expected);
    CHECK(holds(owner, placementsAfter));
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("real G1 retains and restores its complete native pending placement roster",
          "[rollback][placed-enemies][level-opponents][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    LevelOpponents::Resources resources{device, world, weapons, effects, audio, root, 1};
    resources.standOnSight = true;
    opponents.open(resources, players);
    auto& placements = opponents.placements();
    PlacedEnemies::LocalSnapshot initial;
    placements.saveLocal(initial);
    const auto roster = rosterOf(placements);
    const auto pending = pendingIndices(placements);
    const auto enemyBefore = opponents.enemies().checkpoint();
    REQUIRE(enemyBefore);
    REQUIRE_FALSE(pending.empty());
    ViewVolume overhead;
    overhead.forward = {0, -1, 0};
    overhead.up = {0, 0, 1};
    std::optional<Vec3> spot;
    for (const usize index : pending) {
        const auto& candidate = roster[index];
        overhead.position = candidate.spawn.position + Vec3{0, 10, 0};
        placements.observe(overhead, candidate.spawn.position);
        const auto births = placements.activations();
        // This oracle restores the ordinary pool only. Do not mutate a great
        // one's independent pool while comparing a rewound ordinary encounter.
        if (!births.empty() && std::ranges::all_of(births, [](const auto& record) {
                return record.kind < kSwarmKindCount && record.kind != kGarmBroodKind;
            })) {
            spot = candidate.spawn.position;
            break;
        }
    }
    REQUIRE(placements.restoreLocal(initial));
    REQUIRE(spot);
    const auto uploads = device.texturesCreated;
    opponents.watch(overhead, *spot);
    PlacedEnemies::LocalSnapshot expected;
    placements.saveLocal(expected);
    const auto enemyAfter = opponents.enemies().checkpoint();
    REQUIRE(enemyAfter);
    CHECK(placements.pending().size() < pending.size());
    CHECK(rosterOf(placements) == roster);
    REQUIRE(placements.restoreLocal(initial));
    REQUIRE(opponents.enemies().restore(*enemyBefore));
    opponents.watch(overhead, *spot);
    CHECK(holds(placements, expected));
    CHECK(opponents.enemies().checkpoint() == enemyAfter);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native placed activation resumes the same enemy simulation after a delayed correction",
          "[rollback][placed-enemies][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 2, {}, 514);
    REQUIRE(enemies.loadKind(kGruntKind));
    enemies.setContactGridBounds({-100, -10, -100}, {100, 20, 100});
    const auto initial = enemies.checkpoint();
    REQUIRE(initial);
    const std::vector roster{placement({-40, 0, 12}), placement({0, 0, 12}),
                             placement({40, 0, 12})};
    auto pending = roster;
    std::vector<Enemies::Checkpoint> forward;
    std::vector<std::vector<PlacedEnemies::Placement>> waiting;
    const auto uploads = device.texturesCreated;
    const auto observation = [](s32 tick) {
        ViewVolume view;
        // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
        view.position.x = -40 + static_cast<f32>(tick / 12) * 20;
        return view;
    };
    const auto advance = [&](const ViewVolume& view) {
        const std::array players{EnemyView{.player = 0, .position = view.position}};
        enemies.update(1, 1.0f / 60, players, {});
    };
    for (s32 tick = 0; tick < 54; ++tick) {
        const auto view = observation(tick);
        enemies.setView(view);
        originalPhase(pending, view, view.position,
                      [&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        const auto state = enemies.checkpoint();
        REQUIRE(state);
        forward.push_back(*state);
        waiting.push_back(pending);
    }
    REQUIRE(pending.empty());
    CHECK(forward.back().nextInstance == 4);
    CHECK(forward.back().random != initial->random);
    REQUIRE(enemies.restore(*initial));
    PlacedEnemies owner;
    owner.bind(roster);
    PlacedEnemies::LocalSnapshot delayed;
    bool delayedSaved = false;
    for (s32 tick = 0; tick < 54; ++tick) {
        CAPTURE(tick);
        const auto view = observation(tick);
        enemies.setView(view);
        owner.observe(view, view.position);
        owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        CHECK(enemies.checkpoint() == forward[static_cast<usize>(tick)]);
        CHECK(pendingRecords(owner) == waiting[static_cast<usize>(tick)]);
        if (tick == 17) {
            owner.saveLocal(delayed);
            delayedSaved = true;
        }
    }
    REQUIRE(delayedSaved);
    REQUIRE(owner.restoreLocal(delayed));
    REQUIRE(enemies.restore(forward[17]));
    for (s32 tick = 18; tick < 54; ++tick) {
        CAPTURE(tick);
        const auto view = observation(tick);
        enemies.setView(view);
        owner.observe(view, view.position);
        owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        CHECK(enemies.checkpoint() == forward[static_cast<usize>(tick)]);
        CHECK(pendingRecords(owner) == waiting[static_cast<usize>(tick)]);
    }
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native placement rewind replays corrected watch branches without spawning on restore",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies reference;
    PlacedEnemies predicted;
    std::vector<PlacedEnemies::Placement> roster;
    roster.reserve(40);
    for (s32 row = 0; row < 40; ++row) {
        roster.push_back(placement({static_cast<f32>(row * 20), 0, 10}));
    }
    reference.bind(roster);
    predicted.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    std::array<PlacedEnemies::LocalSnapshot, 8> expected;
    PlacedEnemies::LocalSnapshot actual;
    usize callbacks = 0;
    const auto advance = [&](PlacedEnemies& owner, s32 tick) {
        ViewVolume view;
        // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
        view.position = {static_cast<f32>((tick / 4) * 20), 0, 0};
        owner.observe(view, view.position);
        std::vector<PlacedEnemies::Placement> born;
        owner.activate([&](const auto& p) {
            ++callbacks;
            born.push_back(p);
        });
        return born;
    };
    s32 tick = 0;
    for (s32 window = 0; window < 32; ++window) {
        CAPTURE(window, tick);
        predicted.saveLocal(saved);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(predicted, tick + step);
            predicted.saveLocal(expected[static_cast<usize>(step)]);
        }
        REQUIRE(predicted.restoreLocal(saved));
        predicted.observe(std::nullopt, {-900, 0, 0});
        predicted.activate([&](const auto&) { ++callbacks; });
        const auto calls = callbacks;
        REQUIRE(predicted.restoreLocal(saved));
        CHECK(callbacks == calls);
        CHECK(holds(predicted, saved));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            CHECK(advance(predicted, tick) == advance(reference, tick));
            predicted.saveLocal(actual);
            CHECK(predicted.equalLocal(expected[static_cast<usize>(step)], actual));
            CHECK(test::liveDigest(predicted) == test::liveDigest(reference));
        }
        REQUIRE(predicted.canRestoreLocal(saved));
    }
    CHECK(reference.count() < roster.size());
    CHECK(callbacks > 0);
}

TEST_CASE("native placement histories are invalidated only by roster replacement",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies owner;
    PlacedEnemies foreign;
    const std::vector roster{placement({0, 0, 10})};
    owner.bind(roster);
    foreign.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    CHECK(owner.equalLocal(saved, saved));
    CHECK_FALSE(foreign.equalLocal(saved, saved));
    CHECK_FALSE(foreign.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    owner.bind(roster);
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    owner.clear();
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
}

TEST_CASE("native placement equality includes every view field and unseen attention",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 100})});
    ViewVolume view;
    owner.observe(view, {});
    PlacedEnemies::LocalSnapshot before;
    PlacedEnemies::LocalSnapshot after;
    owner.saveLocal(before);
    const s32 change = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    Vec3 attention{0};
    switch (change) {
    case 0: view.position.x += 1; break;
    case 1: view.right.x += 0.01f; break;
    case 2: view.up.y += 0.01f; break;
    case 3: view.forward.z += 0.01f; break;
    case 4: view.sinX += 0.01f; break;
    case 5: view.cosX -= 0.01f; break;
    case 6: view.sinY += 0.01f; break;
    case 7: view.cosY -= 0.01f; break;
    case 8: attention.x = 1; break;
    default: break;
    }
    owner.observe(change == 9 ? std::nullopt : std::optional{view}, attention);
    owner.saveLocal(after);
    CHECK_FALSE(owner.equalLocal(before, after));
    CHECK(owner.count() == 1);
    REQUIRE(owner.restoreLocal(before));
    owner.saveLocal(after);
    CHECK(owner.equalLocal(before, after));
}

} // namespace
