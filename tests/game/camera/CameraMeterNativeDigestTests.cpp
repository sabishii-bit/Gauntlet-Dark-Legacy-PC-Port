#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ItemArchive.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/camera/TowerCamera.h"
#include "game/hud/BossMeter.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class Owner>
NativeStateDigest::Digest liveHash(const Owner& owner,
                                   NativeStateDigest::Resources resources = {}) {
    NativeStateDigest value(resources);
    REQUIRE(owner.appendNativeDigest(value));
    REQUIRE(value.checkedFinish());
    return *value.checkedFinish();
}

template <class Owner, class Snapshot>
NativeStateDigest::Digest savedHash(const Owner& owner, const Snapshot& saved,
                                    NativeStateDigest::Resources resources = {}) {
    NativeStateDigest value(resources);
    REQUIRE(owner.appendLocalDigest(value, saved));
    REQUIRE(value.checkedFinish());
    return *value.checkedFinish();
}

HealthMeterDefinition meterDefinition() {
    HealthMeterDefinition meter;
    meter.pieces = 2;
    meter.advance = 256;
    meter.leftInset = 44;
    meter.rightInset = 53;
    meter.shown = true;
    meter.backed = true;
    return meter;
}

TEST_CASE("native camera digest includes every stored camera and smoothing field",
          "[native-state-digest][camera-meter-native-digest]") {
    TowerCamera camera;
    const auto initial = camera.localState();
    auto changed = initial;
    const auto field = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    switch (field) {
    case 0: changed.camera.position.x = 1; break;
    case 1: changed.camera.pitch = 0.2f; break;
    case 2: changed.camera.yaw = 0.2f; break;
    case 3: changed.camera.roll = 0.2f; break;
    case 4: changed.attention.z = 3; break;
    case 5: changed.distance = 32; break;
    case 6: changed.marker = 1; break;
    case 7: changed.aimedMarker = 2; break;
    case 8: changed.yawRate = 0.1f; break;
    case 9: changed.pitchRate = 0.1f; break;
    case 10: changed.turnStep = 1; break;
    case 11: changed.ringPositions[5].y = 7; break;
    case 12: changed.ringDistances[4] = 7; break;
    case 13: changed.ringIndex = 1; break;
    default: break;
    }
    CHECK_FALSE(TowerCamera::equalLocal(initial, changed));
    CHECK(savedHash(camera, initial) != savedHash(camera, changed));
    camera.restoreLocal(changed);
    CHECK(liveHash(camera) == savedHash(camera, changed));
    camera.restoreLocal(initial);
    CHECK(liveHash(camera) == savedHash(camera, initial));
    // Diagnostic owner/generation metadata is not a native camera value.
    const TowerCamera copied = camera;
    CHECK(liveHash(copied) == liveHash(camera));
}

TEST_CASE("native camera digest survives corrected turns and independent presentation cadence",
          "[native-state-digest][camera-meter-native-digest]") {
    std::array<WorldLocator, 2> markers;
    markers[0].position = {0, 0, 0};
    markers[0].rotation = {0.4f, 0, 0};
    markers[1].position = {100, 0, 0};
    markers[1].rotation = {0.8f, 1.3f, 0};
    CameraRange range;
    range.boundsMin = {-20, -20, -20};
    range.boundsMax = {120, 20, 20};
    const CameraView view;
    std::array<CameraSubject, 4> party;
    for (auto& player : party) {
        player.feet = {0, 0, 0};
        player.follow = {0, 3, 0};
    }
    TowerCamera reference;
    TowerCamera predicted;
    reference.reset(party, markers, range, view);
    predicted.reset(party, markers, range, view);
    const auto step = [&](TowerCamera& camera, s32 tick, bool wrong) {
        for (usize seat = 0; seat < party.size(); ++seat) {
            const f32 across = tick % 80 < 40 ? 95.0f : 0.0f;
            const f32 spread = static_cast<f32>(seat * 3);
            party[seat].feet = {across + (wrong ? -spread : spread), 0, wrong ? -8.0f : spread - 4};
            party[seat].follow = party[seat].feet + Vec3{0, 3, 0};
        }
        camera.update(party, markers, range, view, 1.0f / 60);
        if (tick % 19 == 7) {
            camera.snapAttention(party, range);
        }
    };
    bool sawTurn = false;
    s32 tick = 0;
    for (s32 correction = 0; correction < 32; ++correction) {
        const s32 distance = correction % 8 + 1;
        const auto saved = predicted.localState();
        const auto before = liveHash(predicted);
        for (s32 offset = 0; offset < distance; ++offset) {
            step(predicted, tick + offset, true);
        }
        CHECK(savedHash(predicted, saved) == before);
        predicted.restoreLocal(saved);
        REQUIRE(liveHash(predicted) == before);
        for (s32 offset = 0; offset < distance; ++offset) {
            step(reference, tick + offset, false);
            step(predicted, tick + offset, false);
            sawTurn = sawTurn || predicted.turning();
            const auto beforePresentation = liveHash(predicted);
            for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
                const auto interpolated = predicted.camera().interpolate(saved.camera, alpha);
                (void)interpolated.clipTransform(0.9f, 1920, 1080, Mat4{1});
            }
            CHECK(liveHash(predicted) == beforePresentation);
            CHECK(liveHash(predicted) == liveHash(reference));
            CHECK(TowerCamera::equalLocal(predicted.localState(), reference.localState()));
        }
        tick += distance;
    }
    CHECK(sawTurn);
}

TEST_CASE("native meter digest includes hidden fresh state and immutable layout",
          "[native-state-digest][camera-meter-native-digest]") {
    BossMeter first;
    BossMeter second;
    REQUIRE(first.bind(meterDefinition(), nullptr));
    REQUIRE(second.bind(meterDefinition(), nullptr));
    CHECK(liveHash(first) == liveHash(second));
    BossMeter::LocalSnapshot fresh;
    first.saveLocal(fresh);
    const auto initial = liveHash(first);
    first.update(0, 0, 1, true, false);
    CHECK(first.shown() == second.shown());
    CHECK(first.fillWidths() == second.fillWidths());
    CHECK(liveHash(first) != liveHash(second)); // only the first-update latch changed
    CHECK(savedHash(first, fresh) == initial);
    REQUIRE(first.restoreLocal(fresh));
    CHECK(liveHash(first) == initial);
    auto definition = meterDefinition();
    const auto field = GENERATE(0, 1, 2, 3, 4, 5);
    s32 left = 0;
    switch (field) {
    case 0: definition.name = "HEAD"; break;
    case 1: definition.pieces = 1; break;
    case 2: left = 24; break;
    case 3: definition.leftInset = 45; break;
    case 4: definition.rightInset = 54; break;
    case 5: definition.backed = false; break;
    default: break;
    }
    REQUIRE(second.bind(definition, nullptr, left));
    CHECK(first.shown() == second.shown());
    CHECK(liveHash(first) != liveHash(second));
    NativeStateDigest foreign;
    CHECK_FALSE(second.appendLocalDigest(foreign, fresh));
    CHECK_FALSE(foreign.checkedFinish());
    first.clear();
    NativeStateDigest stale;
    CHECK_FALSE(first.appendLocalDigest(stale, fresh));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("native multipart meter digest restores every layer after one to eight wrong ticks",
          "[native-state-digest][camera-meter-native-digest]") {
    std::array<HealthMeterReading, 3> readings;
    for (usize i = 0; i < readings.size(); ++i) {
        readings[i].definition = meterDefinition();
        readings[i].definition.backed = i == 0;
        readings[i].health = readings[i].maxHealth = 1200 + static_cast<f32>(i) * 300;
    }
    BossMeters reference;
    BossMeters predicted;
    reference.bind(readings, nullptr);
    predicted.bind(readings, nullptr);
    const auto step = [&](BossMeters& meters, s32 tick, bool wrong) {
        auto frame = readings;
        frame[0].health -= static_cast<f32>(tick);
        frame[1].health = wrong ? 1400.0f : 600.0f;
        frame[2].health = wrong ? 0.0f : 500.0f;
        meters.update(1, frame, !wrong, wrong);
    };
    s32 tick = 0;
    for (s32 distance = 1; distance <= 8; ++distance) {
        BossMeters::LocalSnapshot saved;
        predicted.saveLocal(saved);
        const auto before = liveHash(predicted);
        REQUIRE(savedHash(predicted, saved) == before);
        for (s32 i = 0; i < distance; ++i) {
            step(predicted, tick + i, true);
        }
        CHECK(savedHash(predicted, saved) == before);
        REQUIRE(predicted.restoreLocal(saved));
        for (s32 i = 0; i < distance; ++i) {
            step(reference, tick + i, false);
            step(predicted, tick + i, false);
            CHECK(liveHash(predicted) == liveHash(reference));
        }
        tick += distance;
    }
    BossMeters::LocalSnapshot saved;
    predicted.saveLocal(saved);
    NativeStateDigest foreign;
    CHECK_FALSE(reference.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    predicted.bind(std::span{readings}.first(1), nullptr);
    NativeStateDigest rebound;
    CHECK_FALSE(predicted.appendLocalDigest(rebound, saved));
    CHECK_FALSE(rebound.checkedFinish());
}

TEST_CASE("native meter texture-bank identities agree across loads without render side effects",
          "[native-state-digest][camera-meter-native-digest][assets]") {
    const auto path = test::assetOrSkip("MONSTERS/LICH/textures.ngc").parent_path();
    ItemArchive firstArchive;
    ItemArchive secondArchive;
    REQUIRE(firstArchive.load(path));
    REQUIRE(secondArchive.load(path));
    BossMeter first;
    BossMeter second;
    REQUIRE(first.bind(meterDefinition(), &firstArchive.textures));
    REQUIRE(second.bind(meterDefinition(), &secondArchive.textures));
    first.update(0, 3000, 3000, true, false);
    second.update(0, 3000, 3000, true, false);
    NativeStateDigest missing;
    CHECK_FALSE(first.appendNativeDigest(missing));
    CHECK_FALSE(missing.checkedFinish());
    NativeStateResources a;
    NativeStateResources b;
    REQUIRE(a.registerArchive(firstArchive, "boss/LICH"));
    REQUIRE(b.registerArchive(secondArchive, "boss/LICH"));
    REQUIRE(liveHash(first, a.resources()) == liveHash(second, b.resources()));
    BossMeter::LocalSnapshot saved;
    first.saveLocal(saved);
    const auto before = liveHash(first, a.resources());
    first.update(8, 0, 3000, false, true);
    CHECK(savedHash(first, saved, a.resources()) == before);
    test::FakeRenderDevice device;
    REQUIRE(first.restoreLocal(saved));
    CHECK(device.texturesCreated == 0);
    CHECK(device.textureUpdates == 0);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    first.draw(canvas, device);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    CHECK(liveHash(first, a.resources()) == before);
    REQUIRE(a.refreshArchive(firstArchive));
    CHECK(liveHash(first, a.resources()) == liveHash(second, b.resources()));
    first.clear();
    second.clear();
}

TEST_CASE("native camera and meter digest bytes stay pinned for distinct bound values",
          "[native-state-digest][camera-meter-native-digest][native-digest-pin]") {
    TowerCamera camera;
    auto state = camera.localState();
    state.camera.position = {1.5f, 2.25f, -3};
    state.camera.pitch = 0.25f;
    state.camera.yaw = 0.5f;
    state.camera.roll = 0.125f;
    state.attention = {4, 5, 6};
    state.distance = 32;
    state.marker = 1;
    state.aimedMarker = 2;
    state.yawRate = 0.0625f;
    state.pitchRate = 0.03125f;
    state.turnStep = 7;
    state.ringPositions[5] = {7, 8, 9};
    state.ringDistances[4] = 11;
    state.ringIndex = 4;
    camera.restoreLocal(state);
    auto definition = meterDefinition();
    definition.name = "HEAD";
    BossMeter meter;
    REQUIRE(meter.bind(definition, nullptr, 24));
    std::array<HealthMeterReading, 2> readings;
    for (usize i = 0; i < readings.size(); ++i) {
        readings[i].definition = meterDefinition();
        readings[i].definition.backed = i == 0;
        readings[i].health = 900 + static_cast<f32>(i) * 150;
        readings[i].maxHealth = 1200 + static_cast<f32>(i) * 300;
    }
    BossMeters meters;
    meters.bind(readings, nullptr);
    // SHA-256 of the native digest of each owner above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kCamera =
        "d699c9f25ce608fad16fb662e349784ab661849579d371c62529a1102fc7b634";
    constexpr std::string_view kMeter =
        "7cef43ce69e4aa7977010d9be97b45399ee4bffc41815146c147f0025eae1ad4";
    constexpr std::string_view kMeters =
        "c1c26a2546005b286aecf7f8c64b4ebe01b887eb58e624a47a62d9538d8856c3";
    CHECK(test::hexBytes(liveHash(camera)) == kCamera);
    CHECK(test::hexBytes(liveHash(meter)) == kMeter);
    CHECK(test::hexBytes(liveHash(meters)) == kMeters);
}

} // namespace
