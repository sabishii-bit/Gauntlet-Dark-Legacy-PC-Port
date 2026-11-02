#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/combat/MissileTarget.h"
#include "game/party/PlayerMissiles.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;

TEST_CASE("weapon replay admits authored elemental strong and ordinary spread modifiers",
          "[game][missiles][weapon-replay][elemental-prediction]") {
    for (u32 element = 0; element < 16; ++element) {
        for (const u32 flags :
             {0U, PlayerMissiles::kPlainModifiers, PlayerMissiles::kStrongThrowFlags,
              PlayerMissiles::kPlainModifiers | PlayerMissiles::kStrongThrowFlags}) {
            CHECK(PlayerMissiles::replayModifiers(flags | element) == (element <= 4));
        }
    }
    for (u32 bit = 4; bit < 32; ++bit) {
        const u32 flag = 1U << bit;
        CHECK(PlayerMissiles::replayModifiers(flag) ==
              ((flag & PlayerMissiles::kReplayModifiers) != 0));
    }
}

TEST_CASE("native flight retains a value copy of its immutable release provenance",
          "[game][missiles][weapon-replay]") {
    PlayerMissiles pool;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(1);
    launch.position = {0, 8, 0};
    launch.release = MissileRelease{1, 72, true};
    const auto expected = launch.release;
    REQUIRE(pool.launch(launch));
    launch.release.reset();
    REQUIRE(pool.missile(0).release == expected);
    pool.update(kStep, nullptr);
    const auto flight = PlayerMissiles::flightCheckpoint(pool.missile(0));
    const u64 instance = pool.missile(0).instance;
    pool.update(kStep, nullptr);
    REQUIRE(pool.restoreFlight(instance, flight));
    CHECK(pool.missile(0).release == expected);
    REQUIRE(pool.launch(launch));
    CHECK_FALSE(pool.missile(1).release);
}

TEST_CASE("native projectile particle history preserves birth and the exact stopped emitter",
          "[game][missiles][particle-flight][elemental-prediction]") {
    PlayerMissiles pool;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(1);
    launch.owner = 2;
    launch.position = {0, 8, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.flags = 1;
    launch.release = MissileRelease{1, 72, true};
    REQUIRE(pool.launch(launch));
    REQUIRE(pool.visualHistory().size() == 1);
    const auto birth = pool.visualHistory().front().flight.birth;
    pool.update(kStep, nullptr);
    REQUIRE(pool.visualHistory().size() == 1);
    const auto active = pool.visualHistory().front();
    CHECK(active.owner == 2);
    CHECK(active.character == 1);
    CHECK(active.release == launch.release);
    CHECK(active.flight.birth == birth);
    CHECK(active.flight.activeSteps == 1);
    CHECK(active.flight.totalSteps == 1);
    CHECK_FALSE(active.flight.retiring);
    const auto target = MissileTarget{4, {0, 7, 1}, 0.1f, 3};
    const std::array targets{target};
    pool.update(kStep, nullptr, targets);
    REQUIRE(pool.count() == 0);
    REQUIRE(pool.visualHistory().size() == 1);
    const auto stopped = pool.visualHistory().front();
    CHECK(stopped.flight.birth == birth);
    CHECK(stopped.flight.activeSteps == 1);
    CHECK(stopped.flight.totalSteps == 2);
    CHECK(stopped.flight.retiring);
    REQUIRE(stopped.flight.valid());
    const auto impacts = pool.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(Vec3{stopped.placement[3]} == impacts.front().position);
    pool.update(kStep, nullptr);
    REQUIRE(pool.visualHistory().size() == 1);
    CHECK(pool.visualHistory().front().flight.totalSteps == 3);
    CHECK(pool.visualHistory().front().flight.activeSteps == 1);
    CHECK(pool.visualHistory().front().placement == stopped.placement);
    CHECK(pool.takeImpacts().empty());
    pool.clear();
    CHECK(pool.visualHistory().empty());
}

TEST_CASE("particle flight history rejects unsafe clocks and private corrections preserve birth",
          "[game][missiles][particle-flight][elemental-prediction]") {
    const PlayerMissiles::ParticleFlight state;
    REQUIRE(state.valid());
    auto invalid = state;
    invalid.stepSeconds = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.birth.age = 0.1f;
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.activeSteps = 1;
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.totalSteps = 1;
    CHECK_FALSE(invalid.valid());
    invalid.retiring = true;
    CHECK(invalid.valid());
    invalid.totalSteps = 4802;
    CHECK_FALSE(invalid.valid());
    PlayerMissiles pool;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 8, 0};
    REQUIRE(pool.launch(launch));
    pool.update(kStep, nullptr);
    const auto history = pool.visualHistory().front().flight;
    const auto checkpoint = PlayerMissiles::flightCheckpoint(pool.missile(0));
    PlayerMissiles replay;
    REQUIRE(replay.launch(launch));
    const auto instance = replay.missile(0).instance;
    REQUIRE(replay.restoreFlight(instance, checkpoint));
    REQUIRE(replay.restoreVisualFlight(instance, history));
    pool.update(kStep, nullptr);
    replay.update(kStep, nullptr);
    CHECK(replay.visualHistory().front().flight == pool.visualHistory().front().flight);
    CHECK(replay.visualHistory().front().placement == pool.visualHistory().front().placement);
    CHECK_FALSE(replay.restoreVisualFlight(instance, invalid));
    replay.update(kStep * 2, nullptr);
    CHECK(replay.visualHistory().empty());
    CHECK(replay.count() == 1);
}

TEST_CASE("weapon release preparation preserves native muzzle hold spread and strong scaling",
          "[game][missiles][weapon-replay]") {
    for (s32 character = 0; character <= 16; ++character) {
        for (const f32 scale : {1.0f, 2.0f}) {
            for (const f32 held : {0.0f, 0.32f, 1.0f}) {
                for (const s32 shots : {1, 3, 5}) {
                    CAPTURE(character, scale, held, shots);
                    PlayerMissiles::WeaponThrow weapon;
                    weapon.owner = 3;
                    weapon.character = character;
                    weapon.position = {10, 5, -7};
                    weapon.center = {10, 9, -7};
                    weapon.hand = {1.25f, -0.5f, 0.75f};
                    weapon.direction = {0.6f, 0, 0.8f};
                    weapon.speed = 47;
                    weapon.damage = 11;
                    weapon.attackSeconds = held;
                    weapon.scale = scale;
                    weapon.flags = 4;
                    weapon.shots = shots;
                    weapon.multiplayer = MultiplayerMode::Stun;
                    const auto launches = PlayerMissiles::weaponLaunches(weapon);
                    REQUIRE(launches.size() == static_cast<usize>(shots));
                    const auto directions = PlayerMissiles::spread(weapon.direction, shots);
                    auto centerSound = MissileWallSound::Level;
                    if (shots == 5) {
                        centerSound = MissileWallSound::FiveWay;
                    } else if (shots == 3) {
                        centerSound = MissileWallSound::ThreeWay;
                    }
                    for (usize i = 0; i < launches.size(); ++i) {
                        const auto& launch = launches[i];
                        CHECK(glm::distance(launch.position, Vec3{12.65f, 8.5f, -5.55f}) <
                              0.00001f);
                        CHECK(launch.direction == directions[i]);
                        CHECK(launch.speed == 47);
                        CHECK(launch.damage == 11 * scale);
                        CHECK(launch.reach == PlayerMissiles::reachFor(held));
                        CHECK(launch.spec == &MissileSpec::of(character));
                        CHECK(launch.owner == 3);
                        CHECK(launch.scale == scale);
                        CHECK(launch.flags == 4);
                        CHECK(launch.multiplayer == MultiplayerMode::Stun);
                        CHECK_FALSE(launch.velocity);
                        CHECK(launch.model == nullptr);
                        CHECK(launch.archive == nullptr);
                        CHECK(launch.textureLender == nullptr);
                        CHECK(launch.riderArchive == nullptr);
                        CHECK(launch.streak.texture == nullptr);
                        CHECK(launch.tree.empty());
                        CHECK(launch.riderTree.empty());
                        CHECK(launch.wallSound ==
                              (i == 0 ? centerSound : MissileWallSound::Silent));
                    }
                }
            }
        }
    }
}

TEST_CASE("assisted prepared volleys share the native muzzle and retain spread after aiming",
          "[game][missiles][weapon-replay]") {
    PlayerMissiles::WeaponThrow weapon;
    weapon.position = {5, 1, 10};
    weapon.center = {5, 5, 10};
    weapon.hand = {1, -1, 0.5f};
    weapon.direction = {-1, 0, 0};
    weapon.speed = 50;
    weapon.target = Vec3{11, 8, 18};
    weapon.shots = 5;
    const auto launches = PlayerMissiles::weaponLaunches(weapon);
    REQUIRE(launches.size() == 5);
    const Vec3 facing{0.6f, 0, 0.8f};
    const Vec3 muzzle = weapon.center + Vec3{0.8f, 0, -0.6f} + Vec3{0, -1, 0} + facing * 2.5f;
    const auto& center = launches.front();
    CHECK(center.position == muzzle);
    REQUIRE(center.velocity);
    const Vec3 distance = *weapon.target - muzzle;
    const f32 flight = std::hypot(distance.x, distance.z) / weapon.speed;
    const Vec3 reached = muzzle + *center.velocity * flight -
                         Vec3{0, 0.5f * center.spec->weight * flight * flight, 0};
    CHECK(glm::distance(reached, *weapon.target) < 0.00001f);
    for (const auto& launch : launches) {
        REQUIRE(launch.velocity);
        CHECK(launch.position == muzzle);
        CHECK(launch.velocity->y == center.velocity->y);
        CHECK(std::hypot(launch.velocity->x, launch.velocity->z) == Approx(weapon.speed));
        const f32 turn = std::atan2(launch.direction.x, launch.direction.z) -
                         std::atan2(center.direction.x, center.direction.z);
        const Vec3 rotated{
            center.velocity->x * std::cos(turn) + center.velocity->z * std::sin(turn),
            center.velocity->y,
            center.velocity->z * std::cos(turn) - center.velocity->x * std::sin(turn)};
        CHECK(*launch.velocity == rotated);
    }
    // A target directly overhead keeps the supplied facing for its muzzle.
    weapon.target = weapon.position + Vec3{0, 9, 0};
    const auto vertical = PlayerMissiles::weaponLaunches(weapon);
    CHECK(vertical.front().direction == weapon.direction);
}

TEST_CASE("cursor volley height correction preserves muzzle yaw and every spread bearing",
          "[game][missiles][cursor-projectile-aim]") {
    for (const f32 height : {1.5f, 9.0f}) {
        CAPTURE(height);
        PlayerMissiles::WeaponThrow weapon;
        weapon.position = {5, 1, 10};
        weapon.center = {5, 7, 10};
        weapon.hand = {1, -1, 0.5f};
        weapon.direction = {0, 0, 1};
        weapon.speed = 50;
        weapon.target = Vec3{6.4f, height, 23};
        weapon.fixedFacing = true;
        weapon.shots = 5;
        const auto launches = PlayerMissiles::weaponLaunches(weapon);
        REQUIRE(launches.size() == 5);
        const Vec3 muzzle{6, 6, 12.5f};
        const auto bearings = PlayerMissiles::spread(weapon.direction, weapon.shots);
        for (usize index = 0; index < launches.size(); ++index) {
            const auto& launch = launches[index];
            REQUIRE(launch.velocity);
            CHECK(launch.position == muzzle);
            CHECK(launch.direction == bearings[index]);
            CHECK(launch.velocity->x == Approx(bearings[index].x * weapon.speed).margin(0.0001f));
            CHECK(launch.velocity->z == Approx(bearings[index].z * weapon.speed).margin(0.0001f));
        }
        const auto& center = launches.front();
        const f32 flight = (weapon.target->z - muzzle.z) / weapon.speed;
        const Vec3 reached = muzzle + *center.velocity * flight -
                             Vec3{0, 0.5f * center.spec->weight * flight * flight, 0};
        CHECK(reached.x == Approx(muzzle.x));
        CHECK(reached.y == Approx(height));
        CHECK(reached.z == Approx(weapon.target->z));

        // Exercise actual collision, not just the calculated ballistic endpoint.
        const std::array targets{MissileTarget{47, {6.4f, height - 0.5f, 23}, 0.75f, 1}};
        PlayerMissiles corrected;
        REQUIRE(corrected.launch(center));
        for (s32 tick = 0; tick < 30; ++tick) {
            corrected.update(kStep, nullptr, targets);
        }
        const auto impacts = corrected.takeImpacts();
        REQUIRE(impacts.size() == 1);
        CHECK(impacts.front().target == 47);
        weapon.target.reset();
        PlayerMissiles unassisted;
        REQUIRE(unassisted.launch(PlayerMissiles::weaponLaunches(weapon).front()));
        for (s32 tick = 0; tick < 30; ++tick) {
            unassisted.update(kStep, nullptr, targets);
        }
        CHECK(unassisted.takeImpacts().empty());
    }
}

TEST_CASE("ordinary flight checkpoints rewind and replay every native class exactly",
          "[game][missiles][weapon-replay]") {
    for (s32 character = 0; character <= 16; ++character) {
        for (const f32 scale : {1.0f, 2.0f}) {
            for (const s32 hz : {30, 60, 120}) {
                CAPTURE(character, scale, hz);
                PlayerMissiles::WeaponThrow weapon;
                weapon.character = character;
                weapon.center = {3, 12, 4};
                weapon.speed = 53;
                weapon.direction = {0.6f, 0, 0.8f};
                weapon.scale = scale;
                weapon.attackSeconds = 0.34f;
                const auto launch = PlayerMissiles::weaponLaunches(weapon).front();
                PlayerMissiles live;
                REQUIRE(live.launch(launch));
                const f32 seconds = 1.0f / static_cast<f32>(hz);
                for (s32 tick = 0; tick < 12; ++tick) {
                    live.update(seconds, nullptr);
                }
                const auto checkpoint = PlayerMissiles::flightCheckpoint(live.missile(0));
                REQUIRE(checkpoint.valid());
                std::vector<PlayerMissiles::FlightCheckpoint> expected;
                for (s32 tick = 0; tick < 24; ++tick) {
                    live.update(seconds, nullptr);
                    expected.push_back(PlayerMissiles::flightCheckpoint(live.missile(0)));
                }
                PlayerMissiles replay;
                REQUIRE(replay.launch(launch));
                const u64 instance = replay.missile(0).instance;
                for (s32 correction = 0; correction < 3; ++correction) {
                    REQUIRE(replay.restoreFlight(instance, checkpoint));
                    CHECK(PlayerMissiles::flightCheckpoint(replay.missile(0)) == checkpoint);
                    CHECK_FALSE(replay.missile(0).presentationCaptured);
                    for (const auto& step : expected) {
                        replay.update(seconds, nullptr);
                        REQUIRE(replay.count() == 1);
                        CHECK(PlayerMissiles::flightCheckpoint(replay.missile(0)) == step);
                    }
                    CHECK(replay.visuals().count() == 0);
                    CHECK(replay.takeImpacts().empty());
                }
            }
        }
    }
}

TEST_CASE("flight restore rejects malformed or stateful shots without changing the pool",
          "[game][missiles][weapon-replay]") {
    PlayerMissiles missiles;
    auto launch = PlayerMissiles::weaponLaunches({}).front();
    REQUIRE(missiles.launch(launch));
    missiles.update(kStep, nullptr);
    const auto baseline = PlayerMissiles::flightCheckpoint(missiles.missile(0));
    const u64 instance = missiles.missile(0).instance;
    std::vector<PlayerMissiles::FlightCheckpoint> invalid;
    auto value = baseline;
    value.position.x = std::numeric_limits<f32>::quiet_NaN();
    invalid.push_back(value);
    value = baseline;
    value.velocity.y = std::numeric_limits<f32>::infinity();
    invalid.push_back(value);
    value = baseline;
    value.velocity.z = 10'001;
    invalid.push_back(value);
    value = baseline;
    value.position.z = 1'000'001;
    invalid.push_back(value);
    value = baseline;
    value.age = -0.1f;
    invalid.push_back(value);
    value.age = PlayerMissiles::kLifeSeconds;
    invalid.push_back(value);
    value = baseline;
    value.scale = 0;
    invalid.push_back(value);
    value.scale = 101;
    invalid.push_back(value);
    value = baseline;
    value.tumble = std::numeric_limits<f32>::quiet_NaN();
    invalid.push_back(value);
    value = baseline;
    value.continuity = 0;
    invalid.push_back(value);
    for (const auto& checkpoint : invalid) {
        CHECK_FALSE(checkpoint.valid());
        CHECK_FALSE(missiles.restoreFlight(instance, checkpoint));
        CHECK(PlayerMissiles::flightCheckpoint(missiles.missile(0)) == baseline);
    }
    CHECK_FALSE(missiles.restoreFlight(instance + 1, baseline));
    for (const auto mode : {MultiplayerMode::Stun, MultiplayerMode::Hurt}) {
        launch.multiplayer = mode;
        REQUIRE(missiles.launch(launch));
        CHECK_FALSE(
            missiles.restoreFlight(missiles.missile(missiles.count() - 1).instance, baseline));
    }
    launch.multiplayer = MultiplayerMode::Normal;
    launch.flags = 4;
    REQUIRE(missiles.launch(launch));
    CHECK_FALSE(missiles.restoreFlight(missiles.missile(missiles.count() - 1).instance, baseline));
    launch.flags = 0;
    launch.potion = 1;
    REQUIRE(missiles.launch(launch));
    CHECK_FALSE(missiles.restoreFlight(missiles.missile(missiles.count() - 1).instance, baseline));
    test::FakeRenderDevice device;
    missiles.bindVisuals(device);
    launch.potion = 0;
    REQUIRE(missiles.launch(launch));
    CHECK_FALSE(missiles.restoreFlight(missiles.missile(0).instance, baseline));
}

TEST_CASE("replayed plain weapons retire at the native wall contact and expiration",
          "[game][missiles][weapon-replay]") {
    CollisionTriangle wall;
    wall.object = 42;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -20, 20}, Vec3{50, -20, 20}, Vec3{0, 80, 20}};
    WorldCollision collision;
    collision.build({wall});
    PlayerMissiles::WeaponThrow weapon;
    weapon.center = {0, 8, 0};
    weapon.speed = 60;
    weapon.scale = 2;
    const auto launch = PlayerMissiles::weaponLaunches(weapon).front();
    for (const bool withWall : {false, true}) {
        PlayerMissiles live;
        PlayerMissiles replay;
        REQUIRE(live.launch(launch));
        live.update(kStep, &collision);
        const auto checkpoint = PlayerMissiles::flightCheckpoint(live.missile(0));
        REQUIRE(replay.launch(launch));
        REQUIRE(replay.restoreFlight(replay.missile(0).instance, checkpoint));
        for (s32 tick = 0; tick < 240 && live.count() > 0; ++tick) {
            const auto* world = withWall ? &collision : nullptr;
            live.update(kStep, world);
            replay.update(kStep, world);
            CHECK(replay.count() == live.count());
            const auto impacts = live.takeImpacts();
            const auto repeated = replay.takeImpacts();
            REQUIRE(impacts.size() == repeated.size());
            if (!impacts.empty()) {
                CHECK(repeated.front().position == impacts.front().position);
                CHECK(repeated.front().worldObject == 42);
                CHECK(repeated.front().damage == impacts.front().damage);
                CHECK(replay.takeImpacts().empty());
            }
        }
        CHECK(live.count() == 0);
        CHECK(replay.count() == 0);
    }
}

TEST_CASE("companion shots pass through potions and still hit the enemy behind them",
          "[game][missiles][shot-potion]") {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true};
    for (const bool breaksPotions : {false, true}) {
        CAPTURE(breaksPotions);
        PlayerMissiles missiles;
        MissileLaunch launch;
        launch.spec = &kSpec;
        launch.position = {0, 1, 0};
        launch.velocity = Vec3{0, 0, 20};
        launch.damage = 10;
        launch.breaksPotions = breaksPotions;
        std::array targets{MissileTarget{1, {0, 0, 3}, 1, 3}, MissileTarget{2, {0, 0, 7}, 1, 3}};
        targets[0].potionBottle = true;
        REQUIRE(missiles.launch(launch));
        missiles.update(0.5f, nullptr, targets);
        const auto impacts = missiles.takeImpacts();
        REQUIRE(impacts.size() == 1);
        CHECK(impacts[0].target == (breaksPotions ? 1 : 2));
        CHECK(impacts[0].damage == 10);
        CHECK(missiles.count() == 0);
    }
}

TEST_CASE("weapon presentation interpolates flight without changing contacts or lifetime",
          "[game][missiles][presentation]") {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 6, 0, false};
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.spec = &kSpec;
    launch.position = {3, 4, 5};
    launch.velocity = Vec3{0, 0, 30};
    REQUIRE(missiles.launch(launch));
    CHECK(Vec3{PlayerMissiles::transformOf(missiles.missile(0), 0)[3]} == launch.position);
    missiles.update(kStep, nullptr);
    const auto& missile = missiles.missile(0);
    REQUIRE(missile.position.z == Approx(5.5f));
    const f32 age = missile.age;
    const f32 tumble = missile.tumble;
    for (const f32 fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.25f}) {
        const Mat4 visual = PlayerMissiles::transformOf(missile, fraction);
        CHECK(visual[3].z == Approx(5 + 0.5f * fraction));
        CHECK(visual[1].y == Approx(std::cos(tumble * fraction)));
        CHECK(missile.position.z == Approx(5.5f));
        CHECK(missile.age == age);
        CHECK(missile.tumble == tumble);
    }
    CHECK(missiles.takeImpacts().empty());
    const std::array<MissileTarget, 1> targets{{{1, {3, 3, 6.5f}, 0.5f, 3}}};
    missiles.update(kStep, nullptr, targets);
    REQUIRE(missiles.count() == 0);
    CHECK(missiles.takeImpacts().size() == 1);
    test::FakeRenderDevice device;
    missiles.draw(device, Mat4{1}, {}, nullptr, 0);
    CHECK(device.draws.empty());
}

TEST_CASE("multiplayer weapon collisions preserve owner immunity and distinct stun policy",
          "[game][missiles][multiplayer-combat]") {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true};
    const std::array<MissilePlayer, 2> players{
        {{3, {0, Vec3{0}, 1, 6}, false}, {1, {0, Vec3{0, 0, 4}, 1, 6}, false}}};
    for (const auto mode :
         {MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt}) {
        CAPTURE(static_cast<s32>(mode));
        PlayerMissiles missiles;
        MissileLaunch launch;
        launch.spec = &kSpec;
        launch.owner = 3;
        launch.position = {0, 3, 0};
        launch.velocity = Vec3{0, 0, 20};
        launch.damage = 12;
        launch.multiplayer = mode;
        launch.playerHitGap = 0.6f;
        REQUIRE(missiles.launch(launch));
        missiles.update(0.3f, nullptr, {}, players);
        const auto hits = missiles.takeImpacts();
        if (mode == MultiplayerMode::Normal) {
            CHECK(hits.empty());
            CHECK(missiles.count() == 1);
        } else {
            REQUIRE(hits.size() == 1);
            CHECK(hits[0].owner == 3);
            CHECK(hits[0].player == 1);
            CHECK(hits[0].target == -1);
            CHECK(hits[0].damage == 12);
            CHECK(hits[0].stun == (mode == MultiplayerMode::Stun));
            CHECK(hits[0].playerHitGap == Approx(0.6f));
            CHECK(hits[0].direction == Vec3{0, 0, 1});
            CHECK(missiles.count() == 0);
        }
    }
}

TEST_CASE("reflective player armor reverses a weapon without changing its owner",
          "[game][missiles][multiplayer-combat]") {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true};
    const std::array<MissilePlayer, 1> players{{{1, {0, Vec3{0, 0, 4}, 1, 6}, true}}};
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.spec = &kSpec;
    launch.owner = 3;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 20};
    launch.damage = 40;
    launch.multiplayer = MultiplayerMode::Stun;
    REQUIRE(missiles.launch(launch));
    missiles.update(0.25f, nullptr, {}, players);
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).owner == 3);
    CHECK(missiles.missile(0).velocity.z == -20);
    CHECK(missiles.missile(0).damage == 15);
    CHECK(missiles.missile(0).age == Approx(1.25f));
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].player == -1);
    CHECK(hits[0].wallSound == MissileWallSound::Ricochet);
}

TEST_CASE("super shots cross multiple player lanes while thrown potions ignore them",
          "[game][missiles][multiplayer-combat]") {
    static constexpr MissileSpec kSpec{"TEST", {}, 0.5f, 0, 0, true};
    const std::array<MissilePlayer, 2> players{
        {{1, {0, Vec3{0, 0, 4}, 1, 6}, false}, {2, {0, Vec3{0, 0, 8}, 1, 6}, false}}};
    for (const bool potion : {false, true}) {
        PlayerMissiles missiles;
        MissileLaunch launch;
        launch.spec = &kSpec;
        launch.owner = 3;
        launch.position = {0, 3, 0};
        launch.velocity = Vec3{0, 0, 20};
        launch.damage = 40;
        launch.flags = 0x100000;
        launch.potion = potion ? 1 : 0;
        launch.multiplayer = MultiplayerMode::Hurt;
        REQUIRE(missiles.launch(launch));
        missiles.update(0.5f, nullptr, {}, players);
        const auto hits = missiles.takeImpacts();
        REQUIRE(hits.size() == (potion ? 0 : 2));
        for (const auto& hit : hits) {
            CHECK(hit.playerHitGap == 1);
        }
        CHECK(missiles.count() == 1);
    }
}

TEST_CASE("reflective super shots spend pass-through on a player impact after this update",
          "[game][missiles][multiplayer-combat]") {
    constexpr u32 kSuper = 0x100000;
    constexpr u32 kReflect = 0x200000;
    for (const bool armor : {false, true}) {
        for (const bool effect : {false, true}) {
            for (const f32 damage : {2.0f, 12.0f}) {
                CAPTURE(armor, effect, damage);
                const MissileSpec spec{"TEST", {}, 0.5f, 0, 0, true, effect ? "HIT" : ""};
                const std::array<MissilePlayer, 1> players{{{1, {0, Vec3{0, 0, 4}, 1, 6}, armor}}};
                PlayerMissiles missiles;
                MissileLaunch launch;
                launch.spec = &spec;
                launch.owner = 3;
                launch.position = {0, 3, 0};
                launch.velocity = Vec3{0, 0, 20};
                launch.damage = damage;
                launch.flags = kSuper | kReflect;
                launch.multiplayer = MultiplayerMode::Hurt;
                REQUIRE(missiles.launch(launch));
                missiles.update(0.15f, nullptr, {}, players);
                REQUIRE(missiles.count() == 1);
                const bool spent = !armor && effect && damage > 2;
                CHECK(((missiles.missile(0).flags & kSuper) == 0) == spent);
                const auto hits = missiles.takeImpacts();
                REQUIRE(hits.size() == 1);
                CHECK(hits[0].effect.empty() == !spent);
                if (spent) {
                    CHECK(hits[0].position == players[0].body.base);
                    // Still within this victim: next update stops even during its local gap.
                    missiles.update(0.01f, nullptr, {}, players);
                    CHECK(missiles.count() == 0);
                } else if (armor) {
                    CHECK(missiles.missile(0).velocity.z == -20);
                }
            }
        }
    }
}

TEST_CASE("missile streak follows velocity and camera with retail head and tail dimensions",
          "[game][missiles][missile-streak]") {
    test::FakeRenderDevice device;
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.spec = &MissileSpec::superShot();
    launch.position = {0, 0, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.streak = {&device.whiteTexture(), Color::rgba(255, 255, 255, 190), 2};
    REQUIRE(missiles.launch(launch));
    missiles.update(0.5f, nullptr);
    CameraFrame camera;
    camera.forward = {0, -1, 0};
    missiles.draw(device, Mat4{1}, {}, &camera);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    REQUIRE(draw.vertices.size() == 6);
    CHECK(draw.texture == launch.streak.texture);
    CHECK(draw.state.blend == BlendMode::Alpha);
    CHECK(draw.state.depthTest);
    CHECK(draw.state.depthWrite);
    CHECK(draw.state.alphaTest == DrawState::kTranslucentAlphaTest);
    // At t=.5: the tail remains at the muzzle and the head leads by two ticks.
    CHECK(glm::distance(draw.vertices[0].position, Vec3{0.5f, 0, 0}) < 0.0001f);
    CHECK(glm::distance(draw.vertices[1].position, Vec3{4, 0, 17}) < 0.0001f);
    CHECK(glm::distance(draw.vertices[2].position, Vec3{-4, 0, 17}) < 0.0001f);
    for (const auto& vertex : draw.vertices) {
        CHECK(vertex.color == launch.streak.color);
    }
    camera.forward = {1, 0, 0};
    device.draws.clear();
    missiles.draw(device, Mat4{1}, {}, &camera);
    CHECK(glm::distance(device.draws.front().vertices[1].position, Vec3{0, 4, 17}) < 0.0001f);
    camera.forward = {0, 0, 1};
    device.draws.clear();
    missiles.draw(device, Mat4{1}, {}, &camera);
    for (const auto& vertex : device.draws.front().vertices) {
        CHECK(std::isfinite(vertex.position.x));
        CHECK(std::isfinite(vertex.position.y));
        CHECK(std::isfinite(vertex.position.z));
    }
    missiles.update(PlayerMissiles::kLifeSeconds, nullptr);
    device.draws.clear();
    missiles.draw(device, Mat4{1}, {}, &camera);
    CHECK(device.draws.empty());
    launch.streak = {};
    REQUIRE(missiles.launch(launch));
    missiles.draw(device, Mat4{1}, {}, &camera);
    CHECK(device.draws.empty());
}

TEST_CASE("Super Shot rolls its flat bolt toward the camera while preserving flight direction",
          "[game][missiles][super-shot-facing][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    PlayerMissiles missiles;
    missiles.bindVisuals(device);
    MissileLaunch launch;
    launch.spec = &MissileSpec::superShot();
    launch.position = {1, 20, 3};
    launch.velocity = Vec3{20, 5, 30};
    launch.archive = &archive;
    launch.tree = "SUPERARROW";
    REQUIRE(missiles.launch(launch));
    for (const f32 yaw : {0.0f, 1.0f, 2.0f}) {
        missiles.update(0.01f, nullptr);
        REQUIRE(missiles.visuals().effect(0).flightDirection == missiles.missile(0).velocity);
        WorldCamera view;
        view.yaw = yaw;
        view.pitch = 0.6f;
        const auto camera = CameraFrame::of(view);
        device.draws.clear();
        missiles.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
        const auto unfaced = device.draws.front().vertices;
        const Mat4 local = glm::inverse(CameraFrame{}.along(
            PlayerMissiles::transformOf(missiles.missile(0)), missiles.missile(0).velocity));
        device.draws.clear();
        missiles.draw(device, Mat4{1}, {}, &camera);
        REQUIRE_FALSE(device.draws.empty());
        const auto expected = camera.along(PlayerMissiles::transformOf(missiles.missile(0)),
                                           missiles.missile(0).velocity);
        REQUIRE(device.draws.front().vertices.size() == unfaced.size());
        for (usize i = 0; i < unfaced.size(); ++i) {
            const Vec3 point{expected * local * Vec4{unfaced[i].position, 1}};
            CHECK(glm::distance(device.draws.front().vertices[i].position, point) < 0.0001f);
        }
    }
    missiles.clear();
    missiles.bindVisuals(device);
    launch.spec = &MissileSpec::of(0); // Tumbling weapons keep their own angular motion.
    REQUIRE(missiles.launch(launch));
    missiles.update(0.01f, nullptr);
    CHECK_FALSE(missiles.visuals().effect(0).flightDirection);
    missiles.clear();
}

TEST_CASE("weapons strike water above the floor without making water walkable",
          "[missiles][projectile-impact]") {
    WorldCollision collision;
    CollisionTriangle water;
    water.object = 42;
    water.objectFlags = WorldCollision::kLiquidSurface;
    water.vertices = {Vec3{-100, 2, -100}, Vec3{100, 2, -100}, Vec3{0, 2, 100}};
    CollisionTriangle floor = water;
    floor.object = 19;
    floor.objectFlags = WorldObject::kFloor;
    for (auto& vertex : floor.vertices) {
        vertex.y = 0;
    }
    collision.build({water, floor});
    REQUIRE(collision.floorAt({0, 3, 0}, 1, 5));
    CHECK(collision.floorAt({0, 3, 0}, 1, 5)->object == 19);
    REQUIRE(collision.projectileFloorAt({0, 3, 0}, 1, 5));
    CHECK(collision.projectileFloorAt({0, 3, 0}, 1, 5)->object == 42);
    PlayerMissiles missiles;
    MissileLaunch shot;
    shot.spec = &MissileSpec::of(0);
    shot.position = {0, 5, 0};
    shot.velocity = Vec3{0, -10, 0};
    REQUIRE(missiles.launch(shot));
    missiles.update(0.5f, &collision);
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 1);
    CHECK(hits.front().liquid);
    CHECK(hits.front().worldObject == 42);
    CHECK(hits.front().target == -1);
    CHECK(missiles.count() == 0);
    collision.setSolid(42, false);
    CHECK(collision.projectileFloorAt({0, 3, 0}, 1, 5)->object == 19);
}

TEST_CASE("gauntlet missiles own independent moving particle trees and retire with live tails",
          "[game][missiles][effects][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELB/ANIM.PS2");
    test::assetOrSkip("PLAYERS/WAR/SFXYEL/ANIM.PS2");
    ItemArchive weapons;
    ItemArchive items;
    ItemArchive player;
    REQUIRE(weapons.load(root / "WEAPONS"));
    REQUIRE(items.load(root / "ITEMS/LEVELB"));
    REQUIRE(player.load(root / "PLAYERS/WAR/SFXYEL"));
    test::FakeRenderDevice device;
    for (const auto* tree : {"BOSSG_ACID", "BOSSG_ELEC"}) {
        INFO(tree);
        PlayerMissiles missiles;
        const std::array<TextureSet*, 1> lenders{&items.textures};
        missiles.bindVisuals(device, lenders);
        MissileLaunch launch;
        launch.spec = &MissileSpec::superShot();
        launch.position = Vec3{0, 20, 0};
        launch.velocity = Vec3{0, 0, 30};
        launch.archive = &weapons;
        launch.tree = tree;
        launch.textureLender = &player.textures;
        REQUIRE(missiles.launch(launch));
        missiles.update(0.1f, nullptr);
        REQUIRE(missiles.visuals().count() == 1);
        const auto& first = missiles.visuals().effect(0);
        REQUIRE(first.particles.field().particleCount() > 0);
        REQUIRE_FALSE(first.particles.field().emitter(0).particles().empty());
        const auto oldOrigin = first.particles.field().emitter(0).particles()[0].origin;
        for (usize i = 0; i < first.particles.field().size(); ++i) {
            const auto& field = first.particles.field();
            const auto& name = field.emitter(i).descriptor().texture;
            CHECK(field.textureOf(i) != &device.whiteTexture());
            if (!weapons.textures.find(name)) {
                TextureSet& owner = player.textures.find(name) ? player.textures : items.textures;
                const auto slot = owner.find(name);
                REQUIRE(slot);
                CHECK(field.textureOf(i) == &owner.texture(device, *slot));
            }
        }
        REQUIRE(missiles.launch(launch));
        missiles.update(1.0f / 30, nullptr);
        REQUIRE(missiles.visuals().count() == 2);
        CHECK(missiles.visuals().effect(0).position.z > missiles.visuals().effect(1).position.z);
        CHECK(missiles.visuals().effect(0).particles.field().emitter(0).age() >
              missiles.visuals().effect(1).particles.field().emitter(0).age());
        CHECK(missiles.visuals().effect(0).particles.field().emitter(0).particles()[0].origin ==
              oldOrigin);
        device.draws.clear();
        missiles.draw(device, Mat4{1}, {});
        REQUIRE(device.draws.size() > 2); // both meshes AND authored particle children
        const std::array<MissileTarget, 1> targets{{{7, {0, 0, 0}, 100, 100}}};
        missiles.update(1.0f / 30, nullptr, targets);
        REQUIRE(missiles.count() == 0);
        REQUIRE(missiles.takeImpacts().size() == 2);
        REQUIRE(missiles.visuals().count() > 0);
        CHECK(missiles.visuals().effect(0).retiring);
        for (s32 i = 0; i < 300; ++i) {
            missiles.update(1.0f / 30, nullptr);
        }
        REQUIRE(missiles.visuals().count() == 0);
        missiles.clear();
    }
}

TEST_CASE("an effect riding a missile goes with it and is finished with it",
          "[game][world][missiles][damage-types]") {
    const auto root = test::scratchDirectory("missile-rider");
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    writeTextFile(root / "models/glow.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
      {"index":0,"name":"GLOW","file":"models/glow.obj","meshTriangles":1}]})");
    writeFile(root / "textures/skin.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"WEAP_TW_R",
       "nodes":[{"name":"XN","object":"GLOW","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":0,"rate":30}]}]})");
    ItemArchive archive;
    test::convertModelFixture(root);
    REQUIRE(archive.load(root));
    test::FakeRenderDevice device;
    PlayerMissiles missiles;
    missiles.bindVisuals(device);
    MissileLaunch launch;
    launch.spec = &MissileSpec::superShot();
    launch.position = Vec3{0, 20, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.riderArchive = &archive;
    launch.riderTree = "WEAP_TW_R";
    REQUIRE(missiles.launch(launch));
    REQUIRE(missiles.count() == 1);
    REQUIRE(missiles.missile(0).rider != 0);
    CHECK(missiles.missile(0).effect == 0); // the weapon itself is a model, not an effect
    REQUIRE(missiles.visuals().count() == 1);
    missiles.update(1.0f, nullptr);
    REQUIRE(missiles.visuals().count() == 1);
    CHECK(missiles.visuals().effect(0).position == missiles.missile(0).position);
    CHECK(missiles.visuals().effect(0).position.z == Approx(30.0f));
    // Stopped, the missile takes its rider with it: a mesh alone leaves nothing behind.
    const std::array<MissileTarget, 1> targets{{{7, {0, 0, 0}, 100, 100}}};
    missiles.update(1.0f / 30, nullptr, targets);
    REQUIRE(missiles.count() == 0);
    CHECK(missiles.visuals().count() == 0);
    missiles.clear();
}

TEST_CASE("super shots pierce bodies once and ignore walls", "[game][items][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::superShot();
    launch.flags = 0x100020;
    launch.damage = 10;
    REQUIRE(missiles.launch(launch));
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -10, 10}, Vec3{50, -10, 10}, Vec3{0, 50, 10}};
    WorldCollision collision;
    collision.build({wall});
    const std::array<MissileTarget, 3> targets{
        {{1, {0, 0, 8}, 1, 8}, {2, {0, 0, 16}, 1, 8}, {3, {0, 0, 16}, 1, 8}}};
    for (s32 i = 0; i < 60; ++i) {
        missiles.update(kStep, &collision, targets);
    }
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).position.z == Approx(30));
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].target == 1);
    CHECK(hits[1].target == 2);
    CHECK(hits[2].target == 3);
    missiles.update(3, &collision, targets);
    CHECK(missiles.count() == 0);
    CHECK(missiles.takeImpacts().empty());
}

TEST_CASE("reflective weapons rebound without creating a damaging wall hit",
          "[game][items][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::of(0);
    launch.flags = 0x200000;
    launch.damage = 10;
    REQUIRE(missiles.launch(launch));
    CollisionTriangle wall;
    wall.object = 73;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -10, 10}, Vec3{50, -10, 10}, Vec3{0, 50, 10}};
    WorldCollision collision;
    collision.build({wall});
    missiles.update(0.5f, &collision);
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).velocity.z < 0);
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].damage == 0);
    CHECK(hits[0].effect.empty());
    CHECK(hits[0].worldObject == 73);
    missiles.update(3, &collision);
    CHECK(missiles.count() == 0);
}

MissileLaunch axeFrom(const Vec3& position) {
    MissileLaunch launch;
    launch.owner = 1;
    launch.position = position;
    launch.direction = Vec3{0.0f, 0.0f, 1.0f};
    launch.speed = 30.0f;
    launch.reach = 15.0f;
    launch.spec = &MissileSpec::of(0);
    return launch;
}

TEST_CASE("each class throws its own weapon, the tier its level earns", "[game][world][missiles]") {
    REQUIRE(MissileSpec::of(0).model == "AXE");
    REQUIRE(MissileSpec::of(0).weight == 12.0f);
    REQUIRE(MissileSpec::of(0).spin > 18.0f);
    REQUIRE_FALSE(MissileSpec::of(0).staysInHand);
    REQUIRE(MissileSpec::of(3).model == "BOW");
    REQUIRE(MissileSpec::of(3).radius == 0.7f);
    REQUIRE(MissileSpec::of(3).spin == 0.0f);
    REQUIRE(MissileSpec::of(3).staysInHand);
    REQUIRE(MissileSpec::of(8).model == "MIN");
    REQUIRE(MissileSpec::of(0).impactTree == "SPARKS");
    REQUIRE(MissileSpec::of(7).impactTree == "EXPSMALL");
    REQUIRE(MissileSpec::of(15).impactTree == "EXPSMALL");
    REQUIRE(MissileSpec::of(16).model == "STF"); // Sumner throws as a wizard
    REQUIRE(MissileSpec::of(-4).model == "AXE");
    bool inCostume = false;
    REQUIRE(MissileSpec::treeName(0, 55, &inCostume) == "AXE_THROW0");
    REQUIRE(inCostume);
    REQUIRE(MissileSpec::treeName(2, 1, &inCostume) == "STF_THROW1");
    REQUIRE_FALSE(inCostume);
    REQUIRE(MissileSpec::treeName(2, 35) == "STF_THROW2");
    REQUIRE(MissileSpec::treeName(2, 99) == "STF_THROW3");
    REQUIRE(MissileSpec::treeName(7, 85) == "BOM_THROW3");
    REQUIRE(MissileSpec::byMagic(2));
    REQUIRE(MissileSpec::byMagic(14));
    REQUIRE_FALSE(MissileSpec::byMagic(0));
}

TEST_CASE("a spread throw fans out fifteen degrees apart, straight on first",
          "[game][world][missiles]") {
    const Vec3 ahead{0.0f, 0.0f, 1.0f};
    REQUIRE(PlayerMissiles::spread(ahead, 1) == std::vector<Vec3>{ahead});
    REQUIRE(PlayerMissiles::spread(ahead, 0).size() == 1);
    const std::vector<Vec3> three = PlayerMissiles::spread(ahead, 3);
    REQUIRE(three.size() == 3);
    REQUIRE(three[0] == ahead);
    REQUIRE(three[1].x == Approx(std::sin(PlayerMissiles::kSpreadStep)));
    REQUIRE(three[2].x == Approx(-std::sin(PlayerMissiles::kSpreadStep)));
    REQUIRE(three[1].z == Approx(std::cos(PlayerMissiles::kSpreadStep)));
    const std::vector<Vec3> five = PlayerMissiles::spread(ahead, 5);
    REQUIRE(five.size() == 5);
    REQUIRE(five[3].x == Approx(0.5f)); // thirty degrees
    REQUIRE(glm::length(five[4]) == Approx(1.0f));
    REQUIRE(PlayerMissiles::spread(ahead, 9).size() == 5);
}

TEST_CASE("strength sets a missile's pace and holding the attack its reach",
          "[game][world][missiles]") {
    REQUIRE(PlayerMissiles::speedFor(0) == 20.0f);
    REQUIRE(PlayerMissiles::speedFor(1000) == 60.0f);
    REQUIRE(PlayerMissiles::speedFor(5000) == 60.0f);
    REQUIRE(PlayerMissiles::speedFor(600) == Approx(44.0f));
    REQUIRE(PlayerMissiles::reachFor(0.2f) == 15.0f);
    REQUIRE(PlayerMissiles::reachFor(0.32f) == Approx(25.0f));
    REQUIRE(PlayerMissiles::reachFor(3.0f) == Approx(35.0f));
}

TEST_CASE("a missile is lobbed to come down half a unit under its start at its reach",
          "[game][world][missiles]") {
    PlayerMissiles missiles;
    REQUIRE_FALSE(missiles.launch(MissileLaunch{}));
    REQUIRE(missiles.launch(axeFrom(Vec3{1.0f, 3.0f, 2.0f})));
    REQUIRE(missiles.count() == 1);
    REQUIRE(missiles.missile(0).owner == 1);
    REQUIRE(missiles.missile(0).velocity.z == 30.0f);
    REQUIRE(missiles.missile(0).velocity.y > 0.0f);
    // Half a second at thirty a second is its reach of fifteen.
    f32 highest = 0.0f;
    for (s32 i = 0; i < 30; ++i) {
        missiles.update(kStep, nullptr);
        highest = std::max(highest, missiles.missile(0).position.y);
    }
    const PlayerMissiles::Missile& axe = missiles.missile(0);
    REQUIRE(axe.position.z == Approx(17.0f).margin(0.01f));
    REQUIRE(axe.position.x == 1.0f);
    REQUIRE(axe.position.y == Approx(2.5f).margin(0.1f));
    REQUIRE(highest > 3.0f);
    REQUIRE(axe.tumble == Approx(18.85f * 0.5f).margin(0.01f));
    // It faces the way it flies and has turned over by its tumble.
    const Mat4 transform = PlayerMissiles::transformOf(axe);
    REQUIRE(Vec3(transform[3]) == axe.position);
    REQUIRE(transform[0].x == Approx(1.0f)); // flying along +z, its side stays its side
    // With nothing to stop it, its time runs out.
    for (s32 i = 0; i < 200; ++i) {
        missiles.update(kStep, nullptr);
    }
    REQUIRE(missiles.count() == 0);
    REQUIRE(missiles.takeImpacts().empty());
}

TEST_CASE("a shot flies through a fence only the party walks against and stops on a "
          "floor-flagged barrier, as WeaponWallCollide's mask has it",
          "[game][world][missiles][wall-mask]") {
    // A 0x100 fence across z = 10 (the party's walls, not a weapon's), then a barrier
    // across z = 20 flagged as floor alone (0x4: a weapon's, not the party's).
    CollisionTriangle fence;
    fence.object = 7;
    fence.objectFlags = 0x100;
    fence.normal = Vec3{0.0f, 0.0f, -1.0f};
    fence.vertices = {Vec3{-20.0f, -5.0f, 10.0f}, Vec3{20.0f, -5.0f, 10.0f},
                      Vec3{0.0f, 40.0f, 10.0f}};
    CollisionTriangle barrier = fence;
    barrier.object = 8;
    barrier.objectFlags = 0x4;
    barrier.vertices = {Vec3{-20.0f, -5.0f, 20.0f}, Vec3{20.0f, -5.0f, 20.0f},
                        Vec3{0.0f, 40.0f, 20.0f}};
    WorldCollision collision;
    collision.build({fence, barrier});
    // The party is stopped by the fence and walks through the barrier.
    CHECK(collision.resolveWalls(Vec3{0.0f, 1.0f, 9.8f}, 0.75f, 1.0f, 4.0f).z < 9.8f);
    CHECK(collision.resolveWalls(Vec3{0.0f, 1.0f, 19.8f}, 0.75f, 1.0f, 4.0f).z == 19.8f);
    PlayerMissiles missiles;
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (s32 i = 0; i < 60 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &collision);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<MissileImpact> impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts[0].position.z > 18.0f);
    CHECK(impacts[0].position.z < 20.5f);
    CHECK(impacts[0].worldObject == 8);
}

TEST_CASE("walls and floors stop a missile where it strikes", "[game][world][missiles]") {
    // A wall across z = 10 facing the thrower, and a floor far below.
    CollisionTriangle wall;
    wall.object = 42;
    wall.normal = Vec3{0.0f, 0.0f, -1.0f};
    wall.vertices = {Vec3{-20.0f, -5.0f, 10.0f}, Vec3{20.0f, -5.0f, 10.0f},
                     Vec3{0.0f, 40.0f, 10.0f}};
    WorldCollision collision;
    collision.build({wall});
    PlayerMissiles missiles;
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (s32 i = 0; i < 60 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &collision);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<MissileImpact> impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    REQUIRE(impacts[0].position.z > 8.0f);
    REQUIRE(impacts[0].position.z < 10.5f);
    REQUIRE(impacts[0].owner == 1);
    REQUIRE(impacts[0].potion == 0);
    REQUIRE(impacts[0].effect == "SPARKS");
    REQUIRE(impacts[0].wallSound == MissileWallSound::Level);
    CHECK(impacts[0].worldObject == 42);
    REQUIRE(missiles.takeImpacts().empty());

    // Over a floor, the lob comes down onto it.
    CollisionTriangle floor;
    floor.object = 19;
    floor.normal = Vec3{0.0f, 1.0f, 0.0f};
    floor.vertices = {Vec3{-50.0f, 0.0f, -50.0f}, Vec3{0.0f, 0.0f, 100.0f},
                      Vec3{50.0f, 0.0f, -50.0f}};
    WorldCollision ground;
    ground.build({floor});
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (s32 i = 0; i < 180 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &ground);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<MissileImpact> landed = missiles.takeImpacts();
    REQUIRE(landed.size() == 1);
    CHECK(landed[0].worldObject == 19);
    REQUIRE(landed[0].position.z > 15.0f); // past its reach, the floor lower than its drop
    REQUIRE(landed[0].position.y < 1.5f);
    // A potion flies off at the velocity it is given and says what it was where it lands.
    MissileLaunch toss = axeFrom(Vec3{0.0f, 4.0f, 0.0f});
    toss.spec = &MissileSpec::potion();
    toss.velocity = Vec3{0.0f, 3.5f, 3.5f};
    toss.potion = 4;
    REQUIRE(missiles.launch(toss));
    REQUIRE(missiles.missile(0).velocity == Vec3{0.0f, 3.5f, 3.5f});
    for (s32 i = 0; i < 180 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &ground);
    }
    const std::vector<MissileImpact> burst = missiles.takeImpacts();
    REQUIRE(burst.size() == 1);
    REQUIRE(burst[0].potion == 4);
    // An uncharged throw is deliberately short under the retail weight of 100.
    // The normal held wind-up supplies additional launch speed through the arsenal.
    const f32 flight = (3.5f + std::sqrt(3.5f * 3.5f + 2 * 100 * 3.5f)) / 100;
    REQUIRE(burst[0].position.z == Approx(3.5f * flight).margin(0.1f));
    missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f}));
    missiles.clear();
    REQUIRE(missiles.count() == 0);
}

TEST_CASE("world contacts retain the launched weapon's effect and volley sound policy",
          "[game][world][missiles][projectile-impact]") {
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-20, -5, 10}, Vec3{20, -5, 10}, Vec3{0, 40, 10}};
    WorldCollision collision;
    collision.build({wall});
    PlayerMissiles missiles;
    auto launch = axeFrom({0, 3, 0});
    launch.spec = &MissileSpec::of(7);
    launch.wallSound = MissileWallSound::Silent;
    REQUIRE(missiles.launch(launch));
    missiles.update(0.5f, &collision);
    REQUIRE(missiles.count() == 0);
    const auto impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts.front().target == -1);
    CHECK(impacts.front().effect == "EXPSMALL");
    CHECK(impacts.front().wallSound == MissileWallSound::Silent);
    missiles.update(0.5f, &collision);
    CHECK(missiles.takeImpacts().empty());
}

TEST_CASE("what stands in a missile's way stops it and learns what hit it",
          "[game][world][missiles]") {
    REQUIRE(PlayerMissiles::damageFor(0) == PlayerMissiles::kLeastDamage);
    REQUIRE(PlayerMissiles::damageFor(1000) == PlayerMissiles::kMostDamage);
    REQUIRE(PlayerMissiles::damageFor(400) == 11.0f);
    REQUIRE(PlayerMissiles::damageFor(5000) == PlayerMissiles::kMostDamage);
    PlayerMissiles missiles;
    MissileLaunch launch = axeFrom(Vec3{0.0f, 2.0f, 0.0f});
    launch.damage = 11.0f;
    REQUIRE(missiles.launch(launch));
    // A barrel ahead and one off to the side, which it passes.
    const std::array<MissileTarget, 2> targets{
        MissileTarget{4, Vec3{6.0f, 0.0f, 5.0f}, 1.0f, 3.0f},
        MissileTarget{9, Vec3{0.0f, 0.0f, 8.0f}, 1.0f, 3.0f, {}, 7}};
    std::vector<MissileImpact> impacts;
    for (s32 i = 0; i < 120 && impacts.empty(); ++i) {
        missiles.update(kStep, nullptr, targets);
        impacts = missiles.takeImpacts();
    }
    REQUIRE(impacts.size() == 1);
    REQUIRE(impacts[0].target == 9);
    REQUIRE(impacts[0].node == 7);
    REQUIRE(impacts[0].damage == 11.0f);
    REQUIRE(impacts[0].owner == 1);
    REQUIRE(impacts[0].position.z < 8.0f);
    REQUIRE(impacts[0].position.z > 5.5f);
    REQUIRE(missiles.count() == 0);
    // Thrown over a target, it flies on.
    launch.position = Vec3{0.0f, 9.0f, 0.0f};
    REQUIRE(missiles.launch(launch));
    for (s32 i = 0; i < 20; ++i) {
        missiles.update(kStep, nullptr, targets);
    }
    REQUIRE(missiles.takeImpacts().empty());
    REQUIRE(missiles.count() == 1);
}

TEST_CASE("a strong throw's weapon flies twice the size", "[game][world][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch = axeFrom(Vec3{0.0f, 2.0f, 0.0f});
    launch.scale = 2.0f;
    REQUIRE(missiles.launch(launch));
    REQUIRE(missiles.missile(0).scale == 2.0f);
    const Mat4 placed = PlayerMissiles::transformOf(missiles.missile(0));
    REQUIRE(glm::length(Vec3{placed[0]}) == Catch::Approx(2.0f));
    launch.scale = 1.0f;
    REQUIRE(missiles.launch(launch));
    REQUIRE(glm::length(Vec3{PlayerMissiles::transformOf(missiles.missile(1))[0]}) ==
            Catch::Approx(1.0f));
}

} // namespace
