#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/camera/CameraMovementLimit.h"
#include "game/camera/TowerCamera.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

CameraSubject standing(const Vec3& feet) {
    return {feet, feet + Vec3{0, 2.5f, 0}, 1.25f};
}
struct CameraFixture {
    test::FakeRenderDevice device;
    std::filesystem::path root;
    LevelWorld world;
    const LevelRef level{"TEST", 0, "CAMERA", "Camera checkpoint", "", "", "", 0};
    const CameraView projection;
    std::array<CameraSubject, 2> party{standing({-2, 0, 0}), standing({2, 0, 0})};
    TowerCamera native;
    TowerCamera replay;

    CameraFixture() : root(test::sampleLevel("tower-camera-checkpoint")) {
        writeTextFile(root / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
          "locators":[
            {"type":"cameraGame","position":[0,0,0],"rotation":[0.5,0,0]},
            {"type":"cameraGame","position":[100,0,0],"rotation":[0.8,1.7,0]},
            {"type":"triggerCamera","position":[50,0,50],"rotation":[0.6,2,0]}]
        })");
        REQUIRE(world.load(device, root, level));
        REQUIRE(world.cameraMarkers().size() == 2);
        native.reset(party, world.cameraMarkers(), world.cameraRange(), projection);
        replay.reset(party, world.cameraMarkers(), world.cameraRange(), projection);
    }
    void update(TowerCamera& camera, f32 seconds) {
        camera.update(party, world.cameraMarkers(), world.cameraRange(), projection, seconds);
    }
    void beginTurn(f32 seconds) {
        party = {standing({86, 0, -2}), standing({94, 0, 3})};
        for (s32 i = 0; i < 3; ++i) {
            update(native, seconds);
        }
        REQUIRE(native.turning());
        REQUIRE(native.marker() == 1);
    }
};

TEST_CASE("camera rewind resumes every native smoothing and turn field at thirty and sixty Hz",
          "[game][camera-local-rewind]") {
    const auto rate = GENERATE(30, 60);
    CAPTURE(rate);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    CameraFixture fixture;
    fixture.beginTurn(seconds);
    const auto baseline = fixture.native.localState();
    REQUIRE(baseline.turnStep > 0);
    REQUIRE(baseline.turnStep < TowerCamera::kTurnSteps);
    fixture.replay.restoreLocal(baseline);
    REQUIRE(TowerCamera::equalLocal(fixture.replay.localState(), baseline));
    const auto textures = fixture.device.texturesCreated;
    bool finishedTurn = false;
    bool wrappedRing = false;
    for (s32 tick = 0; tick < rate * 3; ++tick) {
        CAPTURE(tick);
        const f32 step = static_cast<f32>(tick) / static_cast<f32>(rate);
        fixture.party = {standing({90 + step * 3, step, -4 - step}),
                         standing({94 - step, 0, 4 + step * 2})};
        fixture.update(fixture.native, seconds);
        fixture.update(fixture.replay, seconds);
        const auto& expected = fixture.native.localState();
        CHECK(TowerCamera::equalLocal(fixture.replay.localState(), expected));
        wrappedRing = wrappedRing || expected.ringIndex < baseline.ringIndex;
        finishedTurn = finishedTurn || !fixture.native.turning();
    }
    CHECK(wrappedRing);
    CHECK(finishedTurn);
    CHECK(fixture.device.texturesCreated == textures);
}

TEST_CASE("camera rewind preserves subsequent native leash and camera relative directions",
          "[game][camera-local-rewind][camera-limit]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    CameraFixture fixture;
    fixture.beginTurn(seconds);
    const auto baseline = fixture.native.localState();
    for (s32 tick = 0; tick < rate; ++tick) {
        fixture.party = {standing({-100, 0, -20}), standing({100, 0, 80})};
        fixture.update(fixture.native, seconds);
    }
    REQUIRE_FALSE(TowerCamera::equalLocal(fixture.native.localState(), baseline));
    fixture.native.restoreLocal(baseline);
    fixture.replay.restoreLocal(baseline);
    CHECK(TowerCamera::equalLocal(fixture.native.localState(), baseline));
    bool limited = false;
    for (s32 tick = 0; tick < 120; ++tick) {
        CAPTURE(rate, tick);
        const Vec3 before = fixture.native.attention() + Vec3{60, -2.5f, 0};
        const Vec3 desired = before + Vec3{0.2f, 0, 0.1f};
        const Vec3 expected = CameraMovementLimit::constrain(
            before, desired, fixture.native.attention(), fixture.native.camera(),
            fixture.projection, {0, 2.5f, 0});
        const Vec3 actual = CameraMovementLimit::constrain(
            before, desired, fixture.replay.attention(), fixture.replay.camera(),
            fixture.projection, {0, 2.5f, 0});
        CHECK(actual == expected);
        CHECK(fixture.replay.camera().right() == fixture.native.camera().right());
        CHECK(fixture.replay.camera().forward() == fixture.native.camera().forward());
        limited = limited || expected != desired;
        fixture.party = {standing(expected),
                         standing(fixture.native.attention() + Vec3{-20, 0, 0})};
        fixture.update(fixture.native, seconds);
        fixture.update(fixture.replay, seconds);
        CHECK(TowerCamera::equalLocal(fixture.replay.localState(), fixture.native.localState()));
    }
    CHECK(limited);
}

TEST_CASE("camera rewind handles trigger attention snap without discarding native history",
          "[game][camera-local-rewind]") {
    CameraFixture fixture;
    fixture.beginTurn(1.0f / 60);
    const auto before = fixture.native.localState();
    fixture.replay.restoreLocal(before);
    fixture.party = {standing({-30, 3, 4}), standing({20, 0, -3})};
    fixture.native.snapAttention(fixture.party, fixture.world.cameraRange());
    fixture.replay.snapAttention(fixture.party, fixture.world.cameraRange());
    const auto snapped = fixture.native.localState();
    CHECK(snapped.ringDistances == before.ringDistances);
    CHECK(snapped.yawRate == before.yawRate);
    CHECK(snapped.turnStep == before.turnStep);
    CHECK(TowerCamera::equalLocal(fixture.replay.localState(), snapped));
    fixture.update(fixture.native, 1.0f / 60);
    fixture.update(fixture.replay, 1.0f / 60);
    CHECK(TowerCamera::equalLocal(fixture.replay.localState(), fixture.native.localState()));
}
} // namespace
