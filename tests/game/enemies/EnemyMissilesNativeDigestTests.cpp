#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <string_view>
#include <typeindex>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct ModelBinding {
    const TreeModel* model{};
    u32 slot = 2;
    static bool append(NativeStateDigest& digest, std::type_index type, const void* address,
                       void* context) {
        const auto& binding = *static_cast<const ModelBinding*>(context);
        if (type != typeid(TreeModel) || address != binding.model) {
            return false;
        }
        digest.string("fixture/swarm/grunt/projectile");
        digest.word(binding.slot);
        return true;
    }
    NativeStateDigest::Resources resources() { return {this, append}; }
};

NativeStateDigest::Digest nativeHash(const EnemyMissiles& pool,
                                     NativeStateDigest::Resources resources = {}) {
    NativeStateDigest digest(resources);
    REQUIRE(pool.appendNativeDigest(digest));
    REQUIRE(digest.checkedFinish());
    return *digest.checkedFinish();
}

template <class T> NativeStateDigest::Digest valueHash(const T& value) {
    NativeStateDigest digest;
    digest.value(value);
    REQUIRE(digest.checkedFinish());
    return *digest.checkedFinish();
}

TEST_CASE("enemy native digest binds model semantics instead of addresses",
          "[native-digest][enemy-missiles-native-digest]") {
    const TreeModel firstModel{};
    const TreeModel secondModel{};
    ModelBinding first{&firstModel};
    ModelBinding second{&secondModel};
    EnemyMissiles a{71};
    EnemyMissiles b{71};
    const auto kind = EnemyMissileKind::bolt(12, 25, 0.5f);
    a.launch(kind, {0, 3, 0}, {0, 3, 40}, 1, &firstModel, 4);
    b.launch(kind, {0, 3, 0}, {0, 3, 40}, 1, &secondModel, 4);
    REQUIRE(a.count() == 1);
    REQUIRE(b.count() == 1);
    REQUIRE(a.missile(0).model != b.missile(0).model);
    CHECK(nativeHash(a, first.resources()) == nativeHash(b, second.resources()));
    ++second.slot;
    CHECK(nativeHash(a, first.resources()) != nativeHash(b, second.resources()));

    NativeStateDigest unbound;
    CHECK_FALSE(a.appendNativeDigest(unbound));
    CHECK_FALSE(unbound.checkedFinish());
    NativeStateDigest foreignBinding(second.resources());
    CHECK_FALSE(a.appendNativeDigest(foreignBinding));
    CHECK_FALSE(foreignBinding.checkedFinish());

    EnemyMissiles::LocalSnapshot saved;
    a.saveLocal(saved);
    NativeStateDigest local(first.resources());
    REQUIRE(a.appendLocalDigest(local, saved));
    CHECK(local.checkedFinish() == nativeHash(a, first.resources()));
    NativeStateDigest unresolvedLocal;
    CHECK_FALSE(a.appendLocalDigest(unresolvedLocal, saved));
    CHECK_FALSE(unresolvedLocal.checkedFinish());
}

TEST_CASE("enemy native digest sees every lingering burst field",
          "[native-digest][enemy-missiles-native-digest]") {
    EnemyMissiles::Burst base;
    base.blast = {{4, 3, 2}, 7, 12, EnemyBlast::kGas, {0.2f, 0.4f}, 1, true};
    base.stageLeft = 0.15f;
    base.players = {{1, std::numeric_limits<f32>::infinity()}, {2, 0.3f}};
    base.swarm = {{17, 0.6f}};
    base.scenery = {51};
    base.instance = 81;
    const auto baseline = valueHash(base);
    const std::array<std::function<void(EnemyMissiles::Burst&)>, 18> mutations{
        [](auto& v) { v.blast.position.x += 1; },
        [](auto& v) { v.blast.radius += 1; },
        [](auto& v) { v.blast.damage += 1; },
        [](auto& v) { v.blast.flags ^= 1; },
        [](auto& v) { v.blast.stages[1] += 0.1f; },
        [](auto& v) { v.blast.stages.push_back(1); },
        [](auto& v) { ++v.blast.spared; },
        [](auto& v) { v.blast.lit = !v.blast.lit; },
        [](auto& v) { ++v.stage; },
        [](auto& v) { v.stageLeft += 0.01f; },
        [](auto& v) { ++v.players[0].id; },
        [](auto& v) { v.players[1].secondsLeft += 0.01f; },
        [](auto& v) { v.players.push_back({9, 1}); },
        [](auto& v) { ++v.swarm[0].id; },
        [](auto& v) { v.swarm[0].secondsLeft += 0.01f; },
        [](auto& v) { ++v.scenery[0]; },
        [](auto& v) { v.scenery.push_back(82); },
        [](auto& v) { ++v.instance; }};
    for (usize i = 0; i < mutations.size(); ++i) {
        CAPTURE(i);
        auto changed = base;
        mutations[i](changed);
        CHECK(valueHash(changed) != baseline);
    }
}

TEST_CASE("enemy native digest rewinds flight contacts staged blasts RNG and pending outputs",
          "[native-digest][enemy-missiles-native-digest][local-rewind]") {
    const auto ticks = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    EnemyMissiles pool{125};
    auto held = EnemyMissileKind::deathShot();
    held.held = 0.437f;
    pool.launch(held, {0, 3, 0}, {0, 3, 60}, 1, nullptr, 5);
    pool.blast({{12, 3, 0}, 7.5f, 10, EnemyBlast::kGas, {0.27f, 0.43f, 0.33f}, 2, true});
    const std::array players{EnemyView{0, {0, 0, 0.5f}, 1, 6}, EnemyView{2, {12, 0, 0}, 1, 6},
                             EnemyView{3, {13, 0, 0}, 1, 6}};
    const std::array swarm{MissileTarget{9, {12, 0, 1}, 1, 6}};
    auto step = [&](f32 seconds, s32 scenery) {
        pool.update(seconds, nullptr, players, swarm, {},
                    [=](const PickupBlastReach&, std::vector<s32>& touched) {
                        if (std::ranges::find(touched, scenery) == touched.end()) {
                            touched.push_back(scenery);
                        }
                    });
    };
    step(0.173f, 42);
    EnemyMissiles::LocalSnapshot saved;
    pool.saveLocal(saved);
    const auto baseline = nativeHash(pool);
    NativeStateDigest stored;
    REQUIRE(pool.appendLocalDigest(stored, saved));
    CHECK(stored.checkedFinish() == baseline);

    REQUIRE_FALSE(pool.takeHits().empty());
    CHECK(nativeHash(pool) != baseline);
    REQUIRE(pool.restoreLocal(saved));
    REQUIRE_FALSE(pool.takePickupBlasts().empty());
    CHECK(nativeHash(pool) != baseline);
    REQUIRE(pool.restoreLocal(saved));

    for (s32 tick = 0; tick < ticks; ++tick) {
        step(1.0f / 60, 42);
    }
    const auto expected = nativeHash(pool);
    REQUIRE(pool.restoreLocal(saved));
    step(0.7f, 99);
    pool.setShrink(0.5f);
    pool.burst({1, 2, 3}, 8, 20, 0, 2, -1);
    CHECK(nativeHash(pool) != expected);
    REQUIRE(pool.restoreLocal(saved));
    CHECK(nativeHash(pool) == baseline);
    for (s32 tick = 0; tick < ticks; ++tick) {
        step(1.0f / 60, 42);
    }
    CHECK(nativeHash(pool) == expected);
}

TEST_CASE("enemy native digest includes rejected launch RNG and retired birth counters",
          "[native-digest][enemy-missiles-native-digest]") {
    EnemyMissiles pool{771};
    const auto initial = nativeHash(pool);
    EnemyMissiles::LocalSnapshot saved;
    pool.saveLocal(saved);
    EnemyMissileLaunch rejected;
    rejected.body = {0, 3, 0};
    rejected.target = {0, 3, 20};
    rejected.facing = kPi;
    rejected.aimError = 0.8f;
    REQUIRE_FALSE(pool.launch(EnemyMissileKind::arrow(), rejected));
    REQUIRE(pool.count() == 0);
    CHECK(nativeHash(pool) != initial);
    REQUIRE(pool.restoreLocal(saved));
    CHECK(nativeHash(pool) == initial);
    pool.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
    pool.clear();
    REQUIRE(pool.count() == 0);
    CHECK(nativeHash(pool) != initial);
    NativeStateDigest stale;
    const auto untouched = stale.finish();
    CHECK_FALSE(pool.appendLocalDigest(stale, saved));
    CHECK(stale.finish() == untouched);
    const EnemyMissiles foreign;
    NativeStateDigest wrongOwner;
    CHECK_FALSE(foreign.appendLocalDigest(wrongOwner, saved));
    CHECK(wrongOwner.finish() == untouched);
}

TEST_CASE("enemy native digest includes undelivered rock damage",
          "[native-digest][enemy-missiles-native-digest]") {
    EnemyMissiles pool;
    Obstacle box;
    box.centre = {0, 0, 5};
    box.halfAcross = box.halfAlong = 2;
    box.height = 6;
    const std::array stops{MissileStop{.box = box, .rock = 3, .rockHealth = 40}};
    pool.launch(*enemyMissileOf(kGarmBroodKind, EnemyMissileKind::kBolt), {0, 3, 0}, {0, 3, 40}, 1,
                nullptr, 2);
    pool.update(0.1f, nullptr, {}, {}, stops);
    const auto baseline = nativeHash(pool);
    EnemyMissiles::LocalSnapshot saved;
    pool.saveLocal(saved);
    REQUIRE(pool.takeRockHits().size() == 1);
    CHECK(nativeHash(pool) != baseline);
    REQUIRE(pool.restoreLocal(saved));
    CHECK(nativeHash(pool) == baseline);
}

TEST_CASE("enemy native digest bytes stay pinned for a flight and a lingering blast",
          "[native-digest][enemy-missiles-native-digest][native-digest-pin]") {
    const TreeModel model{};
    ModelBinding binding{&model};
    EnemyMissiles pool{125};
    pool.launch(EnemyMissileKind::bolt(12, 25, 0.5f), {0, 3, 0}, {0, 3, 40}, 1, &model, 4);
    pool.blast({{12, 3, 0}, 7.5f, 10, EnemyBlast::kGas, {0.25f, 0.5f}, 2, true});
    pool.setShrink(0.5f);
    REQUIRE(pool.count() == 1);
    // SHA-256 of the native digest of the missile pool above, which netplay peers must hash alike:
    // a literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "fadb2332b74610f4904ff5759ac648e440735696e5c2ec259df2cf8d3d381e26";
    CHECK(test::hexBytes(nativeHash(pool, binding.resources())) == kDigest);
}
} // namespace
