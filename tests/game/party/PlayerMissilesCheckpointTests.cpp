#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/party/PotionEmission.h"
#include "game/party/ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_copy_constructible_v<PlayerMissiles>);
static_assert(!std::is_move_assignable_v<PlayerMissiles>);

void equalPools(const PlayerMissiles& native, const PlayerMissiles& replay,
                std::span<const PlayerMissiles::Resource> resources,
                std::span<const EffectTrees::PoolResource> effects = {}) {
    const auto expected = native.checkpoint(resources, effects);
    const auto actual = replay.checkpoint(resources, effects);
    REQUIRE(expected);
    REQUIRE(actual);
    CHECK(actual == expected);
}

TEST_CASE("full player missile checkpoint resumes piercing contacts reflection and queued impacts",
          "[player-missile-checkpoint]") {
    const auto mode =
        GENERATE(MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt);
    const u32 flags = GENERATE(0U, 0x100000U, 0x200000U, 0x300000U);
    const bool reflectivePlayer = GENERATE(false, true);
    const MissileSpec spec{"CUSTOM", {}, 0.5f, 1.1f, 0, true, "CUSTOM_HIT"};
    const std::array resources{PlayerMissiles::Resource{27, &spec}};
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
    REQUIRE(native.launch(launch));
    // Save after real contact, without draining any queued output.
    native.update(0.25f, &world, targets, players);
    const auto saved = native.checkpoint(resources);
    REQUIRE(saved);
    if (mode != MultiplayerMode::Normal) {
        REQUIRE_FALSE(saved->impacts.empty());
        if (reflectivePlayer) {
            REQUIRE(saved->missiles.size() == 1);
            CHECK(saved->missiles[0].velocity.z < 0);
            CHECK(saved->ricochetIn > 0);
        } else if ((flags & 0x100000) != 0 && (flags & 0x200000) == 0) {
            REQUIRE(saved->missiles.size() == 1);
            CHECK_FALSE(saved->missiles[0].playerContacts.empty());
        }
    }
    REQUIRE(replay.restore(*saved, resources));
    equalPools(native, replay, resources);
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(mode, flags, reflectivePlayer, tick);
        const f32 seconds = tick % 11 == 0 ? 0.0f : 1.0f / 60;
        native.update(seconds, &world, targets, players);
        replay.update(seconds, &world, targets, players);
        equalPools(native, replay, resources);
        if (tick == 20) {
            // Exact correction rewinds contacts and the pending-output queue too.
            REQUIRE(native.restore(*saved, resources));
            REQUIRE(replay.restore(*saved, resources));
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
    equalPools(native, replay, resources);
}

TEST_CASE("native missile history rewinds complete flight contacts effects and undelivered impacts",
          "[local-rewind][missile-local-rewind]") {
    test::ProjectileCheckpointFixture fixture;
    EffectTrees riderPrototype;
    EffectTrees::Setting riderSetting;
    riderSetting.persistent = true;
    riderSetting.missingObjectsAreEmpty = true;
    REQUIRE(riderPrototype.startSet(fixture.device, fixture.archive, "FLIGHT", {}, riderSetting));
    const std::array effects{
        fixture.resources[0],
        EffectTrees::PoolResource{
            2, &fixture.archive, {}, &fixture.textures, &riderPrototype.effect(0)}};
    PlayerMissiles native;
    PlayerMissiles replay;
    native.bindVisuals(fixture.device);
    replay.bindVisuals(fixture.device);
    const bool reflecting = GENERATE(false, true);
    const u32 flags = GENERATE(0U, 0x100000U, 0x200000U, 0x300000U);
    const std::array resources{PlayerMissiles::Resource{27, &MissileSpec::of(0)},
                               PlayerMissiles::Resource{28, &MissileSpec::potion()}};
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
        equalPools(native, replay, resources, effects);
        for (u32 i = 0; i < distance; ++i) {
            step(native, frame + i, false);
            step(replay, frame + i, false);
            equalPools(native, replay, resources, effects);
        }
        (void)native.takeImpacts();
        (void)replay.takeImpacts();
    }
    CHECK(fixture.device.textureUpdates == 0);
}

TEST_CASE(
    "native missile history rejects other pools resource teardown and stale diagnostic restores",
    "[local-rewind][missile-local-rewind]") {
    PlayerMissiles a;
    PlayerMissiles b;
    PlayerMissiles::LocalSnapshot state;
    CHECK_FALSE(a.restoreLocal(state));
    a.saveLocal(state);
    CHECK_FALSE(b.restoreLocal(state));
    const auto diagnostic = a.checkpoint({});
    REQUIRE(diagnostic);
    auto prepared = a.prepareRestore(*diagnostic, {});
    REQUIRE(prepared);
    REQUIRE(a.restoreLocal(state));
    CHECK_FALSE(a.canCommit(*prepared));
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

TEST_CASE("full checkpoint retains native potion history and release metadata across retirement",
          "[player-missile-checkpoint]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const std::array resources{PlayerMissiles::Resource{4, &MissileSpec::potion()}};
    auto launch = PotionEmission::throwPotion({2, {0, 3, 0}, {0, 0, 1}, kind, 24, 1.1f, 0}, 8);
    launch.potionRelease = MissilePotionRelease{61};
    PlayerMissiles native;
    PlayerMissiles replay;
    REQUIRE(native.launch(launch));
    native.update(seconds, nullptr);
    native.update(seconds, nullptr);
    const auto saved = native.checkpoint(resources);
    REQUIRE(saved);
    REQUIRE(saved->history.size() == 1);
    REQUIRE(replay.restore(*saved, resources));
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(kind, seconds, tick);
        native.update(seconds, nullptr);
        replay.update(seconds, nullptr);
        equalPools(native, replay, resources);
    }
    CHECK(native.count() == 0);
    REQUIRE(native.visualHistory().size() == 1);
    CHECK(native.visualHistory()[0].flight.retiring);
    CHECK(native.visualHistory()[0].potionRelease == launch.potionRelease);
}

TEST_CASE("bound player missile pool restores body rider particles and exact render samples",
          "[player-missile-checkpoint][effect-pool-checkpoint]") {
    test::ProjectileCheckpointFixture fixture;
    EffectTrees riderPrototype;
    EffectTrees::Setting riderSetting;
    riderSetting.persistent = true;
    riderSetting.missingObjectsAreEmpty = true;
    REQUIRE(riderPrototype.startSet(fixture.device, fixture.archive, "FLIGHT", {}, riderSetting) !=
            0);
    const std::array effects{
        fixture.resources[0],
        EffectTrees::PoolResource{
            2, &fixture.archive, {}, &fixture.textures, &riderPrototype.effect(0)}};
    auto launch = fixture.launch();
    launch.riderArchive = &fixture.archive;
    launch.riderTree = "FLIGHT";
    launch.streak = {&fixture.device.whiteTexture(), Color::rgba(80, 130, 220, 170), 2.5f};
    const std::array resources{
        PlayerMissiles::Resource{7, launch.spec, nullptr, launch.streak.texture}};
    PlayerMissiles native;
    PlayerMissiles replay;
    native.bindVisuals(fixture.device);
    replay.bindVisuals(fixture.device);
    REQUIRE(native.launch(launch));
    for (s32 tick = 0; tick < 7; ++tick) {
        native.update(1.0f / 60, nullptr);
    }
    const auto saved = native.checkpoint(resources, effects);
    REQUIRE(saved);
    REQUIRE(saved->visuals.effects.size() == 2);
    REQUIRE(saved->visuals.banks.size() == 1);
    const auto uploads = fixture.device.texturesCreated;
    REQUIRE(replay.restore(*saved, resources, effects));
    const std::array targets{MissileTarget{9, {0, 5, 5}, 0.5f, 8}};
    bool sawTail = false;
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick);
        native.update(1.0f / 60, nullptr, targets);
        replay.update(1.0f / 60, nullptr, targets);
        equalPools(native, replay, resources, effects);
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
    const auto retired = native.checkpoint(resources, effects);
    REQUIRE(retired);
    CHECK(retired->visuals.effects.empty());
    REQUIRE(retired->visuals.banks.size() == 1);
    REQUIRE(replay.restore(*retired, resources, effects));
    REQUIRE(native.launch(launch));
    REQUIRE(replay.launch(launch));
    equalPools(native, replay, resources, effects);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("player missile checkpoint preparation is bounded atomic and resource authenticated",
          "[player-missile-checkpoint]") {
    PlayerMissiles pool;
    PlayerMissiles other;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 8, 0};
    launch.release = MissileRelease{0, 72, false};
    const std::array resources{PlayerMissiles::Resource{1, launch.spec}};
    REQUIRE(pool.launch(launch));
    pool.update(1.0f / 60, nullptr);
    const auto saved = pool.checkpoint(resources);
    REQUIRE(saved);
    for (s32 mutation = 0; mutation < 13; ++mutation) {
        CAPTURE(mutation);
        auto bad = *saved;
        switch (mutation) {
        case 0: bad.missiles[0].resource = 2; break;
        case 1: bad.missiles[0].velocity.y = std::numeric_limits<f32>::quiet_NaN(); break;
        case 2: bad.missiles[0].playerContacts = {{1, 0.2f}, {1, 0.3f}}; break;
        case 3: bad.missiles[0].pierced = {5, 5}; break;
        case 4: bad.nextInstance = bad.missiles[0].instance; break;
        case 5: bad.missiles[0].multiplayer = static_cast<MultiplayerMode>(3); break;
        case 6: bad.missiles[0].wallSound = static_cast<MissileWallSound>(5); break;
        case 7: bad.missiles[0].effect = 9; break;
        case 8:
            bad.history[0].flight.birth.velocity.x = std::numeric_limits<f32>::infinity();
            break;
        case 9: bad.history[0].release->generation = 0; break;
        case 10: bad.missiles.resize(PlayerMissiles::Checkpoint::kMaxMissiles + 1); break;
        case 11: bad.visualsBound = true; break;
        case 12: bad.ricochetIn = -1; break;
        default: break;
        }
        CHECK_FALSE(pool.prepareRestore(bad, resources));
        CHECK(pool.checkpoint(resources) == saved);
    }
    CHECK_FALSE(pool.checkpoint({}));
    auto prepared = pool.prepareRestore(*saved, resources);
    auto sibling = pool.prepareRestore(*saved, resources);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(pool.canCommit(*prepared));
    REQUIRE(pool.commitRestore(std::move(*prepared)));
    CHECK_FALSE(pool.commitRestore(std::move(*prepared)));
    CHECK_FALSE(pool.commitRestore(std::move(*sibling)));
    prepared = pool.prepareRestore(*saved, resources);
    REQUIRE(prepared);
    pool.clear();
    CHECK_FALSE(pool.commitRestore(std::move(*prepared)));
    REQUIRE(pool.restore(*saved, resources));
    CHECK(pool.checkpoint(resources) == saved);
}
} // namespace
