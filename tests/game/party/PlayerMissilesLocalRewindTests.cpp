#include <array>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "NativeStateSupport.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PotionEmission.h"
#include "game/party/ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_copy_constructible_v<PlayerMissiles>);
static_assert(!std::is_move_assignable_v<PlayerMissiles>);

/** A bound pool's native digest, its projectile art and the streak's white texture named
 * alike for every pool. */
NativeStateDigest::Digest digestOf(const PlayerMissiles& pool,
                                   const test::ProjectileCheckpointFixture& fixture) {
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(fixture.archive, "stage/projectiles"));
    REQUIRE(resources.registerBuiltinTexture(fixture.device.whiteTexture(), "opaque-white"));
    return test::liveDigest(pool, resources);
}

TEST_CASE("player missile rewind resumes piercing contacts reflection and queued impacts",
          "[missile-local-rewind]") {
    const auto mode =
        GENERATE(MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt);
    const u32 flags = GENERATE(0U, 0x100000U, 0x200000U, 0x300000U);
    const bool reflectivePlayer = GENERATE(false, true);
    const MissileSpec spec{"CUSTOM", {}, 0.5f, 1.1f, 0, true, "CUSTOM_HIT"};
    const std::array targets{MissileTarget{5, {0, 0, 9}, 1, 8}, MissileTarget{6, {0, 0, 12}, 1, 8}};
    const std::array players{MissilePlayer{1, {0, {0, 0, 4}, 1, 8}, reflectivePlayer},
                             MissilePlayer{2, {0, {0, 0, 7}, 1, 8}, false}};
    WorldCollision world;
    CollisionTriangle wall;
    wall.object = 73;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -10, 10}, Vec3{50, -10, 10}, Vec3{0, 50, 10}};
    world.build({wall});
    PlayerMissiles native;
    PlayerMissiles replay;
    MissileLaunch launch;
    launch.spec = &spec;
    launch.owner = 3;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 20};
    launch.damage = 40;
    launch.flags = flags;
    launch.multiplayer = mode;
    launch.playerHitGap = 0.7f;
    launch.breaksPotions = false;
    // Save after real contact, without draining any queued output.
    for (auto* pool : {&native, &replay}) {
        REQUIRE(pool->launch(launch));
        pool->update(0.25f, &world, targets, players);
    }
    PlayerMissiles::LocalSnapshot nativeSaved;
    PlayerMissiles::LocalSnapshot replaySaved;
    native.saveLocal(nativeSaved);
    replay.saveLocal(replaySaved);
    if (mode != MultiplayerMode::Normal) {
        REQUIRE_FALSE(native.takeImpacts().empty());
        REQUIRE(native.restoreLocal(nativeSaved));
        if (reflectivePlayer) {
            REQUIRE(native.count() == 1);
            CHECK(native.missile(0).velocity.z < 0);
        } else if ((flags & 0x100000) != 0 && (flags & 0x200000) == 0) {
            REQUIRE(native.count() == 1);
            CHECK_FALSE(native.missile(0).playerContacts.empty());
        }
    }
    CHECK(test::liveDigest(native) == test::liveDigest(replay));
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(mode, flags, reflectivePlayer, tick);
        const f32 seconds = tick % 11 == 0 ? 0.0f : 1.0f / 60;
        native.update(seconds, &world, targets, players);
        replay.update(seconds, &world, targets, players);
        CHECK(test::liveDigest(native) == test::liveDigest(replay));
        if (tick == 20) {
            // Exact correction rewinds contacts and the pending-output queue too.
            REQUIRE(native.restoreLocal(nativeSaved));
            REQUIRE(replay.restoreLocal(replaySaved));
        }
        if (tick % 17 == 0) {
            const auto expected = native.takeImpacts();
            const auto actual = replay.takeImpacts();
            REQUIRE(expected.size() == actual.size());
            for (usize index = 0; index < actual.size(); ++index) {
                CHECK(actual[index].effect == expected[index].effect);
                CHECK(actual[index].position == expected[index].position);
                CHECK(actual[index].player == expected[index].player);
                CHECK(actual[index].target == expected[index].target);
            }
        }
    }
    REQUIRE(native.launch(launch));
    REQUIRE(replay.launch(launch));
    CHECK(test::liveDigest(native) == test::liveDigest(replay));
}

TEST_CASE("native missile history rewinds complete flight contacts effects and undelivered impacts",
          "[local-rewind][missile-local-rewind]") {
    test::ProjectileCheckpointFixture fixture;
    PlayerMissiles native;
    PlayerMissiles replay;
    native.bindVisuals(fixture.device);
    replay.bindVisuals(fixture.device);
    const bool reflecting = GENERATE(false, true);
    const u32 flags = GENERATE(0U, 0x100000U, 0x200000U, 0x300000U);
    const std::array targets{MissileTarget{5, {0, 0, 5}, 1, 8}, MissileTarget{6, {0, 0, 12}, 1, 8}};
    const std::array players{MissilePlayer{1, {0, {0, 0, 3}, 1, 8}, reflecting},
                             MissilePlayer{2, {0, {0, 0, 7}, 1, 8}, false}};
    std::array<PlayerMissiles::LocalSnapshot, 16> history;
    const auto step = [&](PlayerMissiles& pool, u32 frame, bool wrong) {
        if (frame % 3 == 0) {
            auto launch = fixture.launch();
            launch.owner = 3;
            launch.position = {0, 3, 0};
            launch.velocity = wrong ? Vec3{8, 6, -20} : Vec3{0, 0, 20};
            launch.flags = flags;
            launch.damage = 17;
            launch.multiplayer = MultiplayerMode::Hurt;
            launch.playerHitGap = 0.3f;
            launch.riderArchive = &fixture.archive;
            launch.riderTree = "FLIGHT";
            if (frame % 9 == 0) {
                launch.release.reset();
                launch.spec = &MissileSpec::potion();
                launch.potion = 2;
                launch.potency = 10;
                launch.potionRelease = MissilePotionRelease{frame + 1};
            }
            REQUIRE(pool.launch(launch));
        }
        pool.update(1.0f / 60, nullptr, targets, players);
    };
    for (u32 frame = 0; frame < 64; ++frame) {
        CAPTURE(frame, flags, reflecting);
        auto& saved = history[frame % history.size()];
        replay.saveLocal(saved);
        const u32 distance = frame % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, frame + i, true);
        }
        (void)replay.takeImpacts();
        REQUIRE(replay.restoreLocal(saved));
        CHECK(digestOf(native, fixture) == digestOf(replay, fixture));
        for (u32 i = 0; i < distance; ++i) {
            step(native, frame + i, false);
            step(replay, frame + i, false);
            CHECK(digestOf(native, fixture) == digestOf(replay, fixture));
        }
        (void)native.takeImpacts();
        (void)replay.takeImpacts();
    }
    CHECK(fixture.device.textureUpdates == 0);
}

TEST_CASE("native missile history rejects other pools and resource teardown",
          "[local-rewind][missile-local-rewind]") {
    PlayerMissiles a;
    PlayerMissiles b;
    PlayerMissiles::LocalSnapshot state;
    CHECK_FALSE(a.restoreLocal(state));
    a.saveLocal(state);
    CHECK_FALSE(b.restoreLocal(state));
    REQUIRE(a.restoreLocal(state));
    a.clear();
    CHECK_FALSE(a.restoreLocal(state));
    test::FakeRenderDevice device;
    a.saveLocal(state);
    a.bindVisuals(device);
    CHECK_FALSE(a.restoreLocal(state));
    {
        const PlayerMissiles temporary;
        temporary.saveLocal(state);
    }
    CHECK_FALSE(a.restoreLocal(state));
}

TEST_CASE(
    "player missile rewind retains native potion history and release metadata across retirement",
    "[missile-local-rewind]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    auto launch = PotionEmission::throwPotion({2, {0, 3, 0}, {0, 0, 1}, kind, 24, 1.1f, 0}, 8);
    launch.potionRelease = MissilePotionRelease{61};
    PlayerMissiles native;
    REQUIRE(native.launch(launch));
    native.update(seconds, nullptr);
    native.update(seconds, nullptr);
    REQUIRE(native.visualHistory().size() == 1);
    PlayerMissiles::LocalSnapshot saved;
    native.saveLocal(saved);
    std::vector<NativeStateDigest::Digest> expected;
    for (s32 tick = 0; tick < 190; ++tick) {
        native.update(seconds, nullptr);
        expected.push_back(test::liveDigest(native));
    }
    REQUIRE(native.restoreLocal(saved));
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(kind, seconds, tick);
        native.update(seconds, nullptr);
        CHECK(test::liveDigest(native) == expected[static_cast<usize>(tick)]);
    }
    CHECK(native.count() == 0);
    REQUIRE(native.visualHistory().size() == 1);
    CHECK(native.visualHistory()[0].flight.retiring);
    CHECK(native.visualHistory()[0].potionRelease == launch.potionRelease);
}

TEST_CASE("bound player missile pool restores body rider particles and exact render samples",
          "[missile-local-rewind][effect-local-rewind]") {
    test::ProjectileCheckpointFixture fixture;
    auto launch = fixture.launch();
    launch.riderArchive = &fixture.archive;
    launch.riderTree = "FLIGHT";
    launch.streak = {&fixture.device.whiteTexture(), Color::rgba(80, 130, 220, 170), 2.5f};
    PlayerMissiles native;
    PlayerMissiles replay;
    native.bindVisuals(fixture.device);
    replay.bindVisuals(fixture.device);
    for (auto* pool : {&native, &replay}) {
        REQUIRE(pool->launch(launch));
        for (s32 tick = 0; tick < 7; ++tick) {
            pool->update(1.0f / 60, nullptr);
        }
    }
    REQUIRE(native.visuals().count() == 2);
    const auto uploads = fixture.device.texturesCreated;
    const std::array targets{MissileTarget{9, {0, 5, 5}, 0.5f, 8}};
    // The replay leaves on a wrong branch, then rewinds to where the native pool stands.
    PlayerMissiles::LocalSnapshot saved;
    replay.saveLocal(saved);
    for (s32 tick = 0; tick < 5; ++tick) {
        replay.update(1.0f / 60, nullptr, targets);
    }
    REQUIRE(digestOf(replay, fixture) != digestOf(native, fixture));
    REQUIRE(replay.restoreLocal(saved));
    bool sawTail = false;
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick);
        native.update(1.0f / 60, nullptr, targets);
        replay.update(1.0f / 60, nullptr, targets);
        CHECK(digestOf(native, fixture) == digestOf(replay, fixture));
        sawTail = sawTail || (native.count() == 0 && native.visuals().count() != 0);
        for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
            const CameraFrame camera;
            fixture.device.draws.clear();
            native.draw(fixture.device, Mat4{1}, {}, &camera, blend);
            const auto expected = fixture.device.draws;
            fixture.device.draws.clear();
            replay.draw(fixture.device, Mat4{1}, {}, &camera, blend);
            REQUIRE(fixture.device.draws.size() == expected.size());
            for (usize index = 0; index < expected.size(); ++index) {
                CHECK(fixture.device.draws[index].vertices == expected[index].vertices);
                CHECK(fixture.device.draws[index].texture == expected[index].texture);
                CHECK(fixture.device.draws[index].state == expected[index].state);
                CHECK(fixture.device.draws[index].transform == expected[index].transform);
            }
        }
    }
    CHECK(sawTail);
    CHECK(native.count() == 0);
    CHECK(native.visuals().count() == 0);
    REQUIRE(native.launch(launch));
    REQUIRE(replay.launch(launch));
    CHECK(digestOf(native, fixture) == digestOf(replay, fixture));
    CHECK(fixture.device.texturesCreated == uploads);
}
} // namespace
