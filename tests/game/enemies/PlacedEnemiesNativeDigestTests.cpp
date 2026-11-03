#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestSupport.h"
#include "game/enemies/PlacedEnemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlacedEnemies::Placement record() {
    PlacedEnemies::Placement result;
    result.kind = kGruntKind;
    result.spawn.position = {0, 0, 100};
    return result;
}

NativeStateDigest::Digest digestOf(const PlacedEnemies& owner) {
    NativeStateDigest digest;
    REQUIRE(owner.appendNativeDigest(digest));
    REQUIRE(digest.checkedFinish());
    return *digest.checkedFinish();
}

NativeStateDigest::Digest digestOf(const PlacedEnemies& owner,
                                   const PlacedEnemies::LocalSnapshot& state) {
    NativeStateDigest digest;
    REQUIRE(owner.appendLocalDigest(digest, state));
    REQUIRE(digest.checkedFinish());
    return *digest.checkedFinish();
}

TEST_CASE("native placement digest covers all roster fields after activation",
          "[placed-enemies-native-digest]") {
    PlacedEnemies a;
    PlacedEnemies b;
    const auto base = record();
    a.bind({base, base});
    a.activate([](const auto&) {});
    auto changed = base;
    const s32 field =
        GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
    switch (field) {
    case 0: changed.spawn.zigZagSide = -1; break;
    case 1: changed.spawn.kind = kDeathKind; break;
    case 2: changed.spawn.tier += 1; break;
    case 3: changed.spawn.algorithm = kChaseWay; break;
    case 4: changed.spawn.position.x += 1; break;
    case 5: changed.spawn.direction.y += 1; break;
    case 6: changed.spawn.clearance += 1; break;
    case 7: changed.spawn.generator = 7; break;
    case 8: changed.spawn.placed = true; break;
    case 9: changed.spawn.patrolBirth = true; break;
    case 10: changed.spawn.allBirthDirections = true; break;
    case 11: changed.spawn.asleep = true; break;
    case 12: changed.spawn.throwInterval += 0.25f; break;
    case 13: changed.spawn.priority = EnemySpawn::Priority::Visible; break;
    case 14: changed.spawn.sight += 1; break;
    case 15: changed.kind = kDeathKind; break;
    case 16: changed.facing += 1; break;
    case 17: changed.viewRadius += 1; break;
    case 18: changed.sight += 1; break;
    case 19: changed.carried = 0; break;
    case 20: changed.carried = 65536; break;
    default: break;
    }
    b.bind({base, changed});
    b.activate([](const auto&) {});
    REQUIRE(a.pending().empty());
    REQUIRE(b.pending().empty());
    CHECK(digestOf(a) != digestOf(b));
}

TEST_CASE("native placement digest retains hidden watch axes angles and attention",
          "[placed-enemies-native-digest]") {
    PlacedEnemies a;
    PlacedEnemies b;
    a.bind({record()});
    b.bind({record()});
    ViewVolume view;
    Vec3 attention{0};
    a.observe(view, attention);
    b.observe(view, attention);
    REQUIRE(digestOf(a) == digestOf(b)); // Independently allocated owners.
    const s32 field =
        GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19);
    if (field < 12) {
        std::array<Vec3*, 4> axes{&view.position, &view.right, &view.up, &view.forward};
        (*axes[static_cast<usize>(field / 3)])[field % 3] += 0.01f;
    } else {
        switch (field) {
        case 12: view.sinX += 0.01f; break;
        case 13: view.cosX += 0.01f; break;
        case 14: view.sinY += 0.01f; break;
        case 15: view.cosY += 0.01f; break;
        case 16: attention.x += 0.01f; break;
        case 17: attention.y += 0.01f; break;
        case 18: attention.z += 0.01f; break;
        default: break;
        }
    }
    b.observe(field == 19 ? std::nullopt : std::optional{view}, attention);
    CHECK(a.count() == b.count());
    CHECK(digestOf(a) != digestOf(b));
    PlacedEnemies::LocalSnapshot saved;
    b.saveLocal(saved);
    CHECK(digestOf(b, saved) == digestOf(b));
    const auto stable = digestOf(b);
    for (s32 query = 0; query < 8; ++query) {
        static_cast<void>(b.inView({1, 2, 3}, 1));
        static_cast<void>(b.activations());
        static_cast<void>(b.roster());
        static_cast<void>(b.pending());
    }
    CHECK(digestOf(b) == stable);
}

TEST_CASE("native placement digest uses the same saved and live visitor through replay",
          "[placed-enemies-native-digest]") {
    PlacedEnemies a;
    PlacedEnemies b;
    std::vector<PlacedEnemies::Placement> roster;
    for (s32 index = 0; index < 10; ++index) {
        auto p = record();
        p.spawn.position = {static_cast<f32>(index) * 100, 0, 10};
        roster.push_back(p);
    }
    a.bind(roster);
    b.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    b.saveLocal(saved);
    const auto initial = digestOf(b, saved);
    const s32 steps = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    const auto tick = [](PlacedEnemies& owner, s32 index) {
        ViewVolume view;
        view.position.x = static_cast<f32>(index) * 100;
        owner.observe(view, view.position);
        std::vector<PlacedEnemies::Placement> births;
        owner.activate([&](const auto& p) { births.push_back(p); });
        return births;
    };
    b.observe(std::nullopt, {100, 200, 300});
    b.activate([](const auto&) {});
    REQUIRE(b.count() == 0);
    CHECK(digestOf(b, saved) == initial); // The saved values do not follow live mutations.
    REQUIRE(b.restoreLocal(saved));
    CHECK(digestOf(b) == initial);
    for (s32 index = 0; index < steps; ++index) {
        CHECK(tick(a, index) == tick(b, index));
        CHECK(digestOf(a) == digestOf(b));
        PlacedEnemies::LocalSnapshot current;
        b.saveLocal(current);
        CHECK(digestOf(b, current) == digestOf(a));
    }
}

TEST_CASE("native placement digest rejects foreign stale and destroyed snapshots",
          "[placed-enemies-native-digest]") {
    auto storage = std::make_unique<std::optional<PlacedEnemies>>();
    auto& owner = storage->emplace();
    owner.bind({record()});
    PlacedEnemies foreign;
    foreign.bind({record()});
    PlacedEnemies::LocalSnapshot saved;
    owner.saveLocal(saved);
    NativeStateDigest rejected;
    CHECK_FALSE(foreign.appendLocalDigest(rejected, saved));
    CHECK_FALSE(rejected.checkedFinish());
    const s32 mutation = GENERATE(0, 1, 2);
    switch (mutation) {
    case 0: owner.clear(); break;
    case 1: owner.bind({record()}); break;
    default:
        storage->reset();
        storage->emplace().bind({record()});
        break;
    }
    NativeStateDigest stale;
    CHECK_FALSE(storage->value().appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
    CHECK_FALSE(storage->value().restoreLocal(saved));
}

TEST_CASE("native placement digest distinguishes empty bound state and roster order",
          "[placed-enemies-native-digest]") {
    PlacedEnemies a;
    PlacedEnemies b;
    b.bind({});
    CHECK(digestOf(a) != digestOf(b));
    auto second = record();
    second.carried = 3;
    a.bind({record(), second});
    b.bind({second, record()});
    CHECK(digestOf(a) != digestOf(b));
    a.clear();
    b.clear();
    CHECK(digestOf(a) == digestOf(b));
}

TEST_CASE("native placement digest bytes stay pinned for an observed roster",
          "[placed-enemies-native-digest][native-digest-pin]") {
    auto first = record();
    first.spawn.tier = 2;
    first.spawn.algorithm = kChaseWay;
    first.spawn.throwInterval = 0.75f;
    first.facing = 0.5f;
    first.viewRadius = 12;
    first.sight = 20;
    first.carried = 3;
    auto second = record();
    second.kind = kDeathKind;
    second.spawn.kind = kDeathKind;
    second.spawn.position = {40, 0, -25};
    second.spawn.asleep = true;
    PlacedEnemies owner;
    owner.bind({first, second});
    ViewVolume view;
    view.position = {1, 2, 3};
    owner.observe(view, Vec3{4, 5, 6});
    // SHA-256 of the native digest of the placements above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "7adf206e3cb7229555b3de627911065b8d66fcf0a2db03b8fd452406e0016442";
    CHECK(test::hexBytes(digestOf(owner)) == kDigest);
}
} // namespace
