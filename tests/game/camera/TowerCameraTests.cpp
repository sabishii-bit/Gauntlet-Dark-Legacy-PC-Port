#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <tuple>
#include <type_traits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/camera/TowerCamera.h"
#include "game/rollback/LocalStateHistory.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

TEST_CASE("returning from a cut snaps attention without resetting camera angles",
          "[game][world][camera]") {
    std::vector<CameraSubject> party{{Vec3{0}, Vec3{0, 2.5f, 0}}};
    TowerCamera camera;
    const CameraRange range;
    const CameraView view;
    camera.reset(party, {}, range, view);
    const f32 yaw = camera.yaw();
    const f32 pitch = camera.pitch();
    party[0] = {Vec3{12, 0, 8}, Vec3{12, 2.5f, 8}};
    camera.snapAttention(party, range);
    CHECK(camera.attention() == party[0].follow);
    CHECK(camera.yaw() == yaw);
    CHECK(camera.pitch() == pitch);
    for (s32 i = 0; i < 30; ++i) {
        camera.update(party, {}, range, view, 1.0f / 30);
        CHECK(glm::distance(camera.attention(), party[0].follow) < 0.001f);
    }
    camera.snapAttention({}, range);
    CHECK(camera.attention() == party[0].follow);
}

WorldLocator marker(const Vec3& position, f32 pitch, f32 yaw, u8 distance = 0) {
    WorldLocator out;
    out.kind = LocatorKind::TriggerCamera;
    out.position = position;
    out.rotation = Vec3{pitch, yaw, 0.0f};
    out.delay = distance;
    return out;
}

CameraSubject standing(f32 x, f32 z) {
    return CameraSubject{Vec3{x, 0.0f, z}, Vec3{x, 2.5f, z}};
}

TEST_CASE("the camera settles behind one player at the marker's angle and the shortest range",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.5f, 0.3f)};
    const std::vector<CameraSubject> party{standing(0.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == 0);
    REQUIRE(camera.yaw() == Approx(0.3f)); // the camera looks the way the marker faces
    REQUIRE(camera.pitch() == Approx(0.5f));
    REQUIRE_FALSE(camera.turning());
    REQUIRE(camera.distance() == Approx(24.0f).margin(0.01f));
    REQUIRE(camera.attention().x == Approx(0.0f).margin(0.01f));
    REQUIRE(camera.attention().y == Approx(2.5f).margin(0.01f)); // the body's centre
    const Vec3 back = camera.attention() - camera.camera().forward() * camera.distance();
    REQUIRE(camera.camera().position.x == Approx(back.x));
    REQUIRE(camera.camera().position.y == Approx(back.y));
    REQUIRE(camera.camera().position.z == Approx(back.z));
    REQUIRE(camera.camera().position.y > 2.5f); // looking down from above

    // Settled, a frame changes nothing.
    REQUIRE_FALSE(camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f));
}

/** Whether `subject` shows whole in `camera`'s frame. */
bool inFrame(const WorldCamera& camera, const CameraView& view, const CameraSubject& subject) {
    const f32 tanX = std::tan(view.horizontalFov * 0.5f);
    const auto shows = [&](const Vec3& point) {
        const Vec3 relative = point - camera.position;
        const f32 depth = glm::dot(relative, camera.forward());
        return depth > 0 && std::abs(glm::dot(relative, camera.right())) <= depth * tanX &&
               std::abs(glm::dot(relative, camera.up())) <= depth * tanX / view.aspect;
    };
    return std::ranges::all_of(std::array{subject.feet, subject.follow}, shows);
}

TEST_CASE("a party carried apart draws the camera out as far as it takes to show everyone",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.2f, 0.0f)};
    std::vector<CameraSubject> party{standing(-2.0f, 0.0f), standing(2.0f, 0.0f)};
    const CameraRange range;
    const CameraView view;
    TowerCamera camera;
    camera.reset(party, markers, range, view);
    // Two players are looked down on by at least the level's minimum pitch.
    REQUIRE(camera.pitch() == Approx(range.minPitch));
    // Close together they still fit at the shortest range.
    REQUIRE(camera.distance() == Approx(24.0f).margin(0.01f));
    REQUIRE(camera.attention().x == Approx(0.0f).margin(0.01f));

    // Set further apart than the party could walk, they draw the camera out past the range
    // (CalcDist never stops at it) and stay in frame.
    party[0] = standing(-30.0f, 0.0f);
    party[1] = standing(30.0f, 0.0f);
    for (s32 i = 0; i < 200; ++i) {
        camera.update(party, markers, range, view, 1.0f / 30.0f);
    }
    CHECK(camera.distance() > range.radiusMax);
    for (const CameraSubject& subject : party) {
        CHECK(inFrame(camera.camera(), view, subject));
    }
    // The view they are held inside as they walk stays where the camera keeps.
    const WorldCamera held = camera.heldView(party.size(), markers, range);
    CHECK(glm::distance(held.position, camera.attention()) ==
          Approx(range.radiusMax * TowerCamera::kPartyDistance));
    CHECK(held.yaw == camera.yaw());
    CHECK(held.pitch == camera.pitch());
    CHECK(glm::dot(camera.attention() - held.position, held.forward()) ==
          Approx(range.radiusMax * TowerCamera::kPartyDistance));
}

TEST_CASE("the camera keeps the range's longest for a party, a marker's own distance, and the "
          "shortest for one player",
          "[game][world][camera]") {
    const CameraRange range;
    std::vector<WorldLocator> markers{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.2f, 0.0f)};
    std::vector<CameraSubject> party{standing(-2.0f, 0.0f), standing(2.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, range, CameraView{});
    // fn_8006DF34: maxrad times 0.9375 for a party, minrad times 0.8 alone (0x8006E1EC-0x8006E26C).
    CHECK(camera.keptDistance(2, markers, range) == range.radiusMax * TowerCamera::kPartyDistance);
    CHECK(camera.keptDistance(1, markers, range) == range.radiusMin * TowerCamera::kAloneDistance);
    CHECK(camera.keptDistance(2, {}, range) == range.radiusMax * TowerCamera::kPartyDistance);
    // A level whose range has no span keeps its shortest.
    CameraRange fixedRange = range;
    fixedRange.radiusMax = fixedRange.radiusMin;
    CHECK(camera.keptDistance(2, markers, fixedRange) == fixedRange.radiusMin);

    // A marker with a fixed distance overrides the range for the party.
    markers[0].delay = 25;
    camera.reset(party, markers, range, CameraView{});
    CHECK(camera.keptDistance(2, markers, range) == 25.0f);
    CHECK(camera.keptDistance(1, markers, range) == range.radiusMin * TowerCamera::kAloneDistance);
    CHECK(camera.distance() == Approx(24.0f).margin(0.01f)); // compact: the shortest range wins
    CHECK(glm::distance(camera.heldView(2, markers, range).position, camera.attention()) ==
          Approx(25.0f));
}

TEST_CASE("near and far players are centred in projected space rather than world space",
          "[game][world][camera][multiplayer]") {
    const std::vector<CameraSubject> party{standing(0, -8), standing(0, 8)};
    TowerCamera camera;
    camera.reset(party, {}, CameraRange{}, CameraView{});
    for (s32 frame = 0; frame < 200; ++frame) {
        camera.update(party, {}, CameraRange{}, CameraView{}, 1.0f / 30.0f);
    }
    const auto height = [&](const CameraSubject& subject) {
        const Vec3 relative = subject.follow - camera.camera().position;
        return glm::dot(relative, camera.camera().up()) /
               glm::dot(relative, camera.camera().forward());
    };
    CHECK(height(party[0]) == Approx(-height(party[1])).margin(1.0e-4f));
}

TEST_CASE("a clearly nearer marker takes over and the camera turns to it over fifty steps",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 0.0f, 0.0f}, 0.3f, kPi),
                                            marker(Vec3{100.0f, 0.0f, 0.0f}, 0.6f, kPi / 2.0f)};
    std::vector<CameraSubject> party{standing(0.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == 0);
    REQUIRE(camera.yaw() == Approx(kPi));

    // Half way between the markers the first still holds; well past it the second wins.
    party[0] = standing(55.0f, 0.0f);
    for (s32 i = 0; i < 300; ++i) {
        camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f);
    }
    REQUIRE(camera.marker() == 0);
    party[0] = standing(90.0f, 0.0f);
    bool turned = false;
    s32 frames = 0;
    for (; frames < 300 && (camera.marker() != 1 || camera.turning()); ++frames) {
        camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f);
        turned = turned || camera.turning();
    }
    REQUIRE(camera.marker() == 1);
    REQUIRE(turned);
    REQUIRE(frames < 300);
    REQUIRE(camera.yaw() == Approx(kPi / 2.0f).margin(1e-4f));
    REQUIRE(camera.pitch() == Approx(0.6f).margin(1e-4f));

    // Without markers the camera looks straight along +z.
    camera.reset(party, {}, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == -1);
    REQUIRE(camera.yaw() == 0.0f);
}

auto cameraValues(const TowerCamera::State& state) {
    return std::tie(state.camera.position, state.camera.pitch, state.camera.yaw, state.camera.roll,
                    state.attention, state.distance, state.marker, state.aimedMarker, state.yawRate,
                    state.pitchRate, state.turnStep, state.ringPositions, state.ringDistances,
                    state.ringIndex);
}

TEST_CASE("native follow-camera value history rewinds marker turns and party smoothing",
          "[local-rewind][camera-local-rewind]") {
    static_assert(std::is_trivially_copyable_v<TowerCamera::State>);
    static_assert(sizeof(TowerCamera::State) <= 256);
    const std::vector<WorldLocator> markers{marker({0, 0, 0}, 0.3f, kPi),
                                            marker({100, 0, 0}, 0.6f, kPi / 2)};
    std::array<CameraSubject, 4> party;
    for (auto& player : party) {
        player = standing(0, 0);
    }
    CameraRange range;
    range.boundsMin = {-20, -20, -20};
    range.boundsMax = {120, 20, 20};
    const CameraView view;
    TowerCamera camera;
    TowerCamera reference;
    camera.reset(party, markers, range, view);
    reference.reset(party, markers, range, view);
    LocalStateHistory<TowerCamera::State> history;
    bool turned = false;
    bool switched = false;
    const auto step = [&](TowerCamera& owner, u64 tick, bool alternative) {
        for (usize seat = 0; seat < party.size(); ++seat) {
            const f32 x = tick % 360 < 180 ? 95.0f : 0.0f;
            const f32 spread = static_cast<f32>((tick / 5 + seat * 4) % 15);
            party[seat] = standing(x + (alternative ? -spread : spread), spread - 7);
        }
        owner.update(party, markers, range, view, 1.0f / 60);
        if (tick % 29 == 13) {
            owner.snapAttention(party, range);
        }
    };
    for (u64 tick = 0; tick < 720; ++tick) {
        REQUIRE(history.capture(tick, [&](auto& saved) {
            saved = camera.localState();
            return true;
        }));
        const auto before = camera.localState();
        const u64 distance = tick % 8 + 1;
        std::array<TowerCamera::State, 8> expected;
        for (u64 frame = 0; frame < distance; ++frame) {
            step(camera, tick + frame, true);
            expected[frame] = camera.localState();
        }
        camera.restoreLocal(*history.find(tick));
        CHECK((cameraValues(camera.localState()) == cameraValues(before)));
        for (u64 frame = 0; frame < distance; ++frame) {
            step(camera, tick + frame, false);
        }
        camera.restoreLocal(*history.find(tick));
        for (u64 frame = 0; frame < distance; ++frame) {
            step(camera, tick + frame, true);
            CHECK((cameraValues(camera.localState()) == cameraValues(expected[frame])));
        }
        camera.restoreLocal(*history.find(tick));
        step(camera, tick, true);
        step(reference, tick, true);
        CHECK((cameraValues(camera.localState()) == cameraValues(reference.localState())));
        turned = turned || camera.turning();
        switched = switched || camera.marker() == 1;
    }
    CHECK(turned);
    CHECK(switched);
}

} // namespace
