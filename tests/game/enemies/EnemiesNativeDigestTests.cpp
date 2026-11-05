#include <array>
#include <filesystem>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Enemies.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;
constexpr std::string_view kPrefix = "stage/enemy-native-fixture/swarm";

std::filesystem::path enemyDigestFixture() {
    const auto root = test::scratchDirectory("enemy-native-digest");
    const auto archive = root / "MONSTERS/GRU";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 1\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[{"name":"BODY","file":"body.obj"}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"name":"SKIN","file":"skin.png","width":2,"height":2},
      {"name":"GLOW","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({
      "textureAnimations":[{"name":"SKIN","texture":0,"source":0,"frames":2,"rate":3}],
      "trees":[{"name":"GRU1","nodes":[
        {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[
          {"name":"READY","frames":12,"frameRate":30},
          {"name":"WALK","frames":8,"frameRate":30,
            "tracks":[{"node":0,"flags":32,"frames":[0,7],"values":[0,1]}]},
          {"name":"ATTACK1","frames":3,"frameRate":30,"repeats":false},
          {"name":"ATTACK1R","frames":3,"frameRate":30,"repeats":false},
          {"name":"HIT1","frames":3,"frameRate":30,"repeats":false},
          {"name":"HIT2","frames":3,"frameRate":30,"repeats":false},
          {"name":"GETUP","frames":3,"frameRate":30,"repeats":false},
          {"name":"DEATH","frames":3,"frameRate":30,"repeats":false}]}]})");
    return root;
}

struct Pool {
    test::FakeRenderDevice device;
    Enemies enemies;
    NativeStateResources resources;
    std::array<EnemyView, 2> players{};

    explicit Pool(const std::filesystem::path& root, u32 seed = 752) {
        enemies.open(device, root, nullptr, 8, {}, seed);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(enemies.loadKind(kItKind)); // generated READY tree; no disc archive
        players[0].player = 0;
        players[0].position = {5, 0, 18};
        players[1].player = 1;
        players[1].position = {-5, 0, 18};
        for (s32 slot = 0; slot < 4; ++slot) {
            EnemySpawn spawn;
            spawn.kind = slot == 3 ? kItKind : kGruntKind;
            spawn.placed = true;
            spawn.asleep = slot == 1;
            spawn.generator = slot;
            spawn.algorithm = kChaseWay;
            spawn.position = {static_cast<f32>(slot - 1) * 10, 0, -8};
            REQUIRE(enemies.spawn(spawn, {}) == slot);
        }
        REQUIRE(enemies.registerNativeResources(resources, kPrefix));
    }

    NativeStateDigest::Digest digest() const {
        NativeStateDigest sink(resources.resources());
        REQUIRE(enemies.appendNativeDigest(sink));
        const auto value = sink.checkedFinish();
        REQUIRE(value);
        return *value;
    }
    NativeStateDigest::Digest digest(const Enemies::LocalSnapshot& saved) const {
        NativeStateDigest sink(resources.resources());
        REQUIRE(enemies.appendLocalDigest(sink, saved));
        const auto value = sink.checkedFinish();
        REQUIRE(value);
        return *value;
    }

    auto visible(s32 slot = 0) const {
        const auto observation = enemies.observe(slot);
        REQUIRE(observation);
        return std::tuple{observation->instance,   observation->health,
                          observation->fullHealth, observation->asleep,
                          observation->alpha,      enemies.positionOf(slot),
                          enemies.yawOf(slot),     enemies.animatorOf(slot)->action()};
    }

    void step(s32 tick, bool wrong = false) {
        players[0].position = {wrong ? -80.0f : static_cast<f32>(tick % 5), 0, 12};
        players[1].position = {5, 0, wrong ? -80.0f : 16.0f};
        if (tick % 5 == 2) {
            EnemyHit hit;
            hit.player = wrong ? 1 : 0;
            hit.damage = wrong ? 1000.0f : 0.5f;
            hit.flags = wrong ? EnemyHit::kMagic : EnemyHit::kKnockBack;
            hit.direction = wrong ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
            (void)enemies.hurt(0, hit);
        }
        if (tick % 7 == 3) {
            enemies.wake(1);
        }
        enemies.setShrink(wrong ? 0.5f : 1);
        enemies.update(1, kStep, players, {}, nullptr, 1, wrong);
        if (wrong) {
            (void)enemies.takeBirths();
            (void)enemies.takeGeneratorEvents();
            (void)enemies.takeFeedback();
            (void)enemies.takeLosses();
        }
    }
};

template <class T>
NativeStateDigest::Digest valueHash(const T& value, NativeStateDigest::Resources resources = {}) {
    NativeStateDigest sink(resources);
    sink.value(value);
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

TEST_CASE("enemy native digests identify independently loaded artwork and ignore drawing cadence",
          "[native-state-digest][enemies-native-digest]") {
    const auto root = enemyDigestFixture();
    Pool first(root);
    Pool second(root);
    REQUIRE(first.enemies.archiveOf(kGruntKind) != second.enemies.archiveOf(kGruntKind));
    REQUIRE(first.enemies.treeOf(kGruntKind, 1) != second.enemies.treeOf(kGruntKind, 1));
    REQUIRE(first.enemies.treeOf(kItKind, 1) != second.enemies.treeOf(kItKind, 1));
    CHECK(first.digest() == second.digest());
    const auto uploads = first.device.texturesCreated;
    bool drew = false;
    for (s32 tick = 0; tick < 48; ++tick) {
        CAPTURE(tick);
        first.step(tick);
        second.step(tick);
        const auto native = first.digest();
        REQUIRE(native == second.digest());
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            first.enemies.draw(first.device, Mat4{1}, WorldLighting{}, nullptr, nullptr, nullptr,
                               alpha);
            first.enemies.drawShadows(first.device, Mat4{1}, Vec3{0, 30, 30}, WorldLighting{},
                                      alpha);
            drew = drew || !first.device.draws.empty();
            first.device.draws.clear();
            CHECK(first.digest() == native);
        }
    }
    CHECK(drew);
    CHECK(first.device.texturesCreated == uploads);
    CHECK(first.device.textureUpdates == 0);
}

TEST_CASE("enemy native digests cover hidden queues grid routes RNG and saved texture clocks",
          "[native-state-digest][enemies-native-digest]") {
    const auto root = enemyDigestFixture();
    Pool pool(root);
    const Pool otherSeed(root, 753);
    CHECK((pool.visible() == otherSeed.visible()));
    CHECK(pool.digest() != otherSeed.digest());
    Enemies::LocalSnapshot original;
    REQUIRE(pool.enemies.saveLocal(original));
    const auto baseline = pool.digest();
    const auto visible = pool.visible();
    const auto change = [&](auto&& action) {
        REQUIRE(pool.enemies.restoreLocal(original));
        action();
        CHECK((pool.visible() == visible));
        CHECK(pool.digest() != baseline);
        CHECK(pool.digest(original) == baseline);
    };
    change([&] { REQUIRE_FALSE(pool.enemies.takeBirths().empty()); });
    change([&] { pool.enemies.setContactGridBounds({-10, 0, -20}, {20, 0, 30}); });
    change([&] { pool.enemies.setLookouts({{{0, 0, 2}, {4, 0, 8}}, {1, 0}}); });
    change([&] {
        MissileTarget target;
        target.id = 75;
        target.base = {50, 0, 10};
        target.radius = 2;
        target.height = 8;
        const std::array targets{target};
        pool.enemies.setCombatantBodies(targets);
    });
    change([&] { pool.enemies.setView(ViewVolume{}); });
    // A held half-step banks time/texture clocks without executing the AI step.
    change([&] { pool.enemies.update(1, kStep, pool.players); });
    REQUIRE(pool.enemies.restoreLocal(original));
    CHECK(pool.digest() == baseline);
    Enemies::LocalSnapshot restored;
    REQUIRE(pool.enemies.saveLocal(restored));
    CHECK(pool.enemies.equalLocal(original, restored));
}

TEST_CASE("enemy native digests match corrected one through eight tick branches",
          "[native-state-digest][enemies-native-digest][local-rewind]") {
    const auto root = enemyDigestFixture();
    Pool reference(root);
    Pool corrected(root);
    for (s32 tick = 0; tick < 12; ++tick) {
        reference.step(tick);
        corrected.step(tick);
    }
    Enemies::LocalSnapshot base;
    REQUIRE(corrected.enemies.saveLocal(base));
    const auto baseDigest = corrected.digest();
    std::array<NativeStateDigest::Digest, 8> expected{};
    std::array<Enemies::LocalSnapshot, 8> exact;
    for (s32 tick = 0; tick < 8; ++tick) {
        reference.step(12 + tick);
        corrected.step(12 + tick);
        expected[static_cast<usize>(tick)] = reference.digest();
        REQUIRE(corrected.enemies.saveLocal(exact[static_cast<usize>(tick)]));
        REQUIRE(corrected.digest() == expected[static_cast<usize>(tick)]);
    }
    for (s32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(corrected.enemies.restoreLocal(base));
        for (s32 tick = 0; tick < distance; ++tick) {
            corrected.step(12 + tick, true);
        }
        CHECK(corrected.digest() != expected[static_cast<usize>(distance - 1)]);
        CHECK(corrected.digest(base) == baseDigest);
        REQUIRE(corrected.enemies.restoreLocal(base));
        for (s32 tick = 0; tick < distance; ++tick) {
            corrected.step(12 + tick);
            CHECK(corrected.digest() == expected[static_cast<usize>(tick)]);
        }
        Enemies::LocalSnapshot replay;
        REQUIRE(corrected.enemies.saveLocal(replay));
        CHECK(corrected.enemies.equalLocal(exact[static_cast<usize>(distance - 1)], replay));
    }
}

TEST_CASE("enemy native digest refuses unknown resources foreign histories and closed bindings",
          "[native-state-digest][enemies-native-digest]") {
    const auto root = enemyDigestFixture();
    Pool first(root);
    const Pool second(root);
    NativeStateDigest unknown;
    CHECK_FALSE(first.enemies.appendNativeDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
    NativeStateResources authoredOnly;
    REQUIRE(authoredOnly.registerArchive(*first.enemies.archiveOf(kGruntKind),
                                         "stage/enemy-native-fixture/swarm/4"));
    NativeStateDigest missingBuiltin(authoredOnly.resources());
    CHECK_FALSE(first.enemies.appendNativeDigest(missingBuiltin));
    CHECK_FALSE(missingBuiltin.checkedFinish());
    Enemies::LocalSnapshot saved;
    REQUIRE(first.enemies.saveLocal(saved));
    NativeStateDigest foreign(second.resources.resources());
    CHECK_FALSE(second.enemies.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    first.enemies.close();
    NativeStateDigest closed(first.resources.resources());
    CHECK_FALSE(first.enemies.appendLocalDigest(closed, saved));
    CHECK_FALSE(closed.checkedFinish());
}

TEST_CASE(
    "native inactive body values and enemy animation requests are not presentation projections",
    "[native-state-digest][enemies-native-digest][enemy-animator-native-digest]") {
    const Enemies::BodyState inactive;
    const auto empty = valueHash(inactive);
    const auto changed = [&](auto&& mutate) {
        auto slot = inactive;
        mutate(slot);
        REQUIRE(slot.state == Enemies::State::Inactive);
        CHECK(valueHash(slot) != empty);
    };
    changed([](auto& slot) { slot.mind.fuse = 3; });
    changed([](auto& slot) { slot.mind.zigZag.hold = 2; });
    changed([](auto& slot) { slot.hurtPending = 4; });
    changed([](auto& slot) { slot.contact = 1; });
    changed([](auto& slot) { slot.burnGap = 0.25f; });
    changed([](auto& slot) { slot.affinity = 2; });
    changed([](auto& slot) { slot.floor = Enemies::BodyState::Floor{2, {1, 3, 4}}; });
    changed([](auto& slot) { slot.deathSkin = "DEATHALT"; });

    const auto root = enemyDigestFixture();
    const Pool first(root);
    const Pool second(root);
    EnemyAnimator animator;
    EnemyAnimator independent;
    REQUIRE(animator.bind(*first.enemies.treeOf(kGruntKind, 1)));
    REQUIRE(independent.bind(*second.enemies.treeOf(kGruntKind, 1)));
    const EnemyAnimator saved = animator;
    const auto initial = valueHash(saved, first.resources.resources());
    CHECK(initial == valueHash(independent, second.resources.resources()));
    CHECK_FALSE(saved.equalLocal(independent)); // exact local equality retains binding guards
    animator.request(EnemyAction::Attack);
    CHECK(animator.action() == saved.action());
    CHECK(animator.player() == saved.player());
    CHECK(valueHash(animator, first.resources.resources()) != initial);
    animator = saved;
    animator.setIdle(0.25f);
    CHECK(valueHash(animator, first.resources.resources()) != initial);
    animator = saved;
    animator.setThrowInterval(0.5f);
    CHECK(valueHash(animator, first.resources.resources()) != initial);
    animator = saved;
    for (s32 tick = 0; tick < 8; ++tick) {
        animator.request(EnemyAction::Walk);
        animator.update(1, kStep);
        const auto beforeDraw = valueHash(animator, first.resources.resources());
        for (const f32 alpha : {0.0f, 0.37f, 1.0f}) {
            TreePose pose;
            animator.evaluatePresentation(pose, alpha);
            (void)animator.presentation().pose(alpha);
            CHECK(valueHash(animator, first.resources.resources()) == beforeDraw);
        }
    }
    animator = saved;
    CHECK(animator.equalLocal(saved));
    CHECK(valueHash(animator, first.resources.resources()) == initial);
    animator.unbind();
    CHECK_FALSE(animator.bound());
    CHECK(valueHash(animator, first.resources.resources()) != valueHash(EnemyAnimator{}));
    NativeStateDigest missingRetainedPose;
    missingRetainedPose.value(animator);
    CHECK_FALSE(missingRetainedPose.checkedFinish());
}

TEST_CASE("enemy native digest bytes stay pinned for a seeded placed roster",
          "[native-state-digest][enemies-native-digest][native-digest-pin]") {
    Pool pool(enemyDigestFixture(), 761);
    pool.enemies.setContactGridBounds({-10, 0, -20}, {20, 0, 30});
    pool.enemies.setLookouts({{{0, 0, 2}, {4, 0, 8}}, {1, 0}});
    // SHA-256 of the native digest of the swarm above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "87ecd5277c25aa14ad099d11be49e144b576a3a07c1ad7cd91a8c462d82c94fa";
    CHECK(test::hexBytes(pool.digest()) == kDigest);
}

TEST_CASE("enemy native digest bytes stay pinned after the chase script and what it saved",
          "[native-state-digest][enemies-native-digest][digest-golden]") {
    Pool pool(enemyDigestFixture());
    for (s32 tick = 0; tick < 12; ++tick) {
        pool.step(tick);
    }
    Enemies::LocalSnapshot saved;
    REQUIRE(pool.enemies.saveLocal(saved));
    for (s32 tick = 12; tick < 20; ++tick) {
        pool.step(tick);
    }
    // SHA-256 of the swarm's native digest twenty ticks in and of what it saved twelve ticks in,
    // which netplay peers must hash alike: a literal moves only when what is visited changes on
    // purpose.
    test::checkPinnedDigests("Enemies", pool.digest(), pool.digest(saved),
                             "85f487e81fb92c246117fceb826b572155a4488a3b642aedf6f5198da7ca6dfc",
                             "3fd78c74d1f37fd7be07c17a191de9bdc63bedca1ad924156987564f580f8fb9");
}
} // namespace
