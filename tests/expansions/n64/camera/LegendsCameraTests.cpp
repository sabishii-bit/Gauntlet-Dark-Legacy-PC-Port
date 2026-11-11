#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <tuple>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "TestSupport.h"
#include "expansions/n64/camera/LegendsCamera.h"
#include "expansions/n64/camera/LegendsCameraChannel.h"
#include "expansions/n64/camera/LegendsCameraRig.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Phase = LegendsCameraChannel::Phase;

constexpr f32 kUpdate = 1.0f / 30;
/// The Tower's camera times: e08694e4's for the shots, case 0x28's for the return.
constexpr LegendsCameraChannel::Times kShotTimes{.acceleration = .7f, .cruise = 1, .braking = .3f};
constexpr LegendsCameraChannel::Times kReturnTimes{
    .acceleration = .5f, .cruise = 1, .braking = .3f};

/** Advances until the channel rests; the advance that reported it, or 0 within `limit`. */
s32 advancesToRest(LegendsCameraChannel& channel, s32 limit = 200) {
    for (s32 update = 1; update <= limit; ++update) {
        if (channel.advance(kUpdate)) {
            return update;
        }
    }
    return 0;
}

/** e0014d04's rows for the angles (x, y, z), game.bin e0014d04..e0014e94. */
std::array<Vec3, 3> nativeRows(f32 x, f32 y, f32 z) {
    const f32 ax = portable::cos(x);
    const f32 bx = portable::sin(x);
    const f32 ay = portable::cos(y);
    const f32 by = portable::sin(y);
    const f32 az = portable::cos(z);
    const f32 bz = portable::sin(z);
    return {Vec3{ay * az + bx * by * bz, -ax * bz, -by * az + ay * bx * bz},
            Vec3{ay * bz - bx * by * az, ax * az, -by * bz - ay * bx * az},
            Vec3{by * ax, bx, ay * ax}};
}

formats::LegendsN64CameraData markers() {
    formats::LegendsN64CameraData data;
    data.markers = {{{0, 0, 0}, 2, .2f}, {{0, 0, 0}, 0, .6f}, {{100, 0, 0}, 1, .7f}};
    return data;
}

formats::LegendsN64CameraData tower() {
    auto data = markers();
    data.tower = true;
    data.towerExits = {{90, 2, -52}, {-90, 5, -52}, {-90, 5, 52}, {90, 5, 52}, {0, -5, 104}};
    return data;
}

void settle(LegendsCamera& camera, const Vec3& feet, s32 frames = 300) {
    for (s32 frame = 0; frame < frames; ++frame) {
        camera.update(feet, 1.0f / 30);
    }
}

TEST_CASE("Legends follow markers exclude the opening view and retain native hysteresis",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(markers(), {0, 0, 0});
    REQUIRE(camera.introActive());
    settle(camera, {0, 0, 0});
    CHECK_FALSE(camera.introActive());
    CHECK(camera.marker() == 1);
    CHECK(camera.camera().yaw == 0);
    CHECK(camera.camera().pitch == Approx(.6f));
    CHECK(camera.distance() == 36);
    settle(camera, {55, 0, 0});
    CHECK(camera.marker() == 1);
    settle(camera, {90, 0, 0});
    CHECK(camera.marker() == 2);
    CHECK(camera.camera().yaw == Approx(1));
    CHECK(camera.camera().pitch == Approx(.7f));
    CHECK(glm::distance(camera.camera().position,
                        camera.attention() - camera.camera().forward() * camera.distance()) <
          .001f);
}

TEST_CASE("N64 boss camera frames its elevated arena focus and every party member",
          "[legends][camera][boss]") {
    auto data = markers();
    data.markers[1].pitch = .1f;
    data.bossFocus = Vec3{-4, 47, -16};
    LegendsCamera camera;
    const Vec3 feet{0, 28, 20};
    camera.reset(data, feet, false);
    CHECK(camera.distance() == 50);
    const Vec3 centre = (feet + *data.bossFocus) * .5f + camera.camera().up() * 3.0f;
    CHECK(glm::distance(camera.attention(), centre) < .001f);
    CHECK(camera.camera().position.y > 40);
    settle(camera, feet);
    CHECK(camera.distance() >= 50);
    CHECK(camera.distance() <= 150);

    std::array<CameraSubject, 4> party{};
    for (auto& subject : party) {
        subject = {feet, feet + Vec3{0, 3, 0}, 3};
    }
    camera.reset(data, party, false);
    const Vec3 partyCentre = (feet * 4.0f + *data.bossFocus) / 5.0f + camera.camera().up() * 3.0f;
    CHECK(glm::distance(camera.attention(), partyCentre) < .001f);
    party[0].feet.x = -60;
    party[3].feet.x = 60;
    for (s32 tick = 0; tick < 600; ++tick) {
        camera.update(party, 1.0f / 30);
    }
    CHECK(camera.distance() > 100);
    CHECK(camera.distance() <= 150);
    CHECK(std::isfinite(camera.camera().position.y));
}

TEST_CASE("N64 boss framing retains fixed tick behavior at different render rates",
          "[legends][camera][boss]") {
    auto data = markers();
    data.bossFocus = Vec3{0, 47, -16};
    const std::array party{CameraSubject{{-20, 28, 20}, {-20, 31, 20}, 3},
                           CameraSubject{{20, 28, 20}, {20, 31, 20}, 3}};
    LegendsCamera thirty;
    LegendsCamera sixty;
    thirty.reset(data, party, false);
    sixty.reset(data, party, false);
    for (s32 tick = 0; tick < 300; ++tick) {
        thirty.update(party, 1.0f / 30);
        sixty.update(party, 1.0f / 60);
        (void)sixty.sample(.001f);
        sixty.update(party, 1.0f / 60);
    }
    CHECK(thirty.camera() == sixty.camera());
    CHECK(thirty.distance() == sixty.distance());
}

TEST_CASE("N64 boss opening leaves provisional focus dormant until the boss wakes",
          "[legends][camera][boss][native-hell2]") {
    auto data = markers();
    data.bossFocus = Vec3{0, -26, -8}; // Hell2's script pose before floor correction.
    data.markers[1] = {{0, -4.5f, 15.25f}, 127 / 40.743664f, 14 / 81.48733f};
    data.markers[2] = data.markers[1];
    const Vec3 feet{0, -4, 25};
    LegendsCamera camera;
    camera.reset(data, feet);
    settle(camera, feet, 90);
    REQUIRE_FALSE(camera.introActive());
    CHECK(camera.distance() == 50);
    CHECK(glm::distance(camera.attention(), feet + camera.camera().up() * 3.0f) < .001f);
    const Vec3 groundedFocus{0, 8, -8};
    camera.trackBoss(groundedFocus, 2.5f);
    settle(camera, feet, 300);
    CHECK(glm::distance(camera.attention(),
                        (feet + groundedFocus) * .5f + camera.camera().up() * 3.0f) < .001f);
}

TEST_CASE("N64 tower camera uses native radial rings and return rays instead of IST angles",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, -20});
    CHECK_FALSE(camera.introActive());
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.camera().pitch == Approx(.05f));
    CHECK(camera.attention().y == 7);
    CHECK(std::hypot(camera.attention().x, camera.attention().z) == Approx(20));
    CHECK(camera.distance() == 55);
    camera.update({0, 0, 46}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    camera.update({0, 0, 47}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    settle(camera, {0, 0, 60});
    CHECK(camera.camera().pitch == Approx(.1f));
    CHECK(camera.distance() == 40);
    CHECK(camera.attention().x == Approx(0).margin(.001f));
    camera.update({0, 0, 43}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    camera.update({0, 0, 42}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    camera.update({0, 0, 91}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    camera.update({0, 0, 88}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    camera.update({0, 0, 87}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    settle(camera, {90, 5, -52});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    CHECK(camera.camera().pitch == Approx(.7f));
    CHECK(camera.distance() == 40);
    CHECK(camera.attention().x / camera.attention().z == Approx(-90.0f / 52));
}

TEST_CASE("Legends camera reformation can reset directly to the nearest follow marker",
          "[legends][camera]") {
    LegendsCamera camera;
    const Vec3 feet{100, 20, 5};
    camera.reset(markers(), feet, false);
    CHECK_FALSE(camera.introActive());
    CHECK(camera.marker() == 2);
    CHECK(camera.camera().yaw == Approx(1));
    CHECK(camera.camera().pitch == Approx(.7f));
    CHECK(camera.distance() == 36);
    CHECK(glm::distance(camera.attention(), feet + camera.camera().up() * 3.0f) < .001f);
    const auto start = camera.camera().position;
    settle(camera, feet);
    CHECK_FALSE(camera.introActive());
    CHECK(glm::distance(camera.camera().position, start) < .001f);
}

TEST_CASE("N64 tower camera uses its own distance limits when entering each radial zone",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, 60});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    CHECK(camera.distance() == 40);
    settle(camera, {0, 0, 60});
    CHECK(camera.distance() == 40);

    camera.reset(tower(), {90, 5, -52});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    CHECK(camera.distance() == 40);
    settle(camera, {90, 5, -52});
    CHECK(camera.distance() == 40);

    settle(camera, {0, 0, 20});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.distance() == 55);
}

TEST_CASE("N64 tower radial transitions use full actor height and stay finite at the origin",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, 0});
    settle(camera, {0, 0, 0});
    CHECK(std::isfinite(camera.camera().position.x));
    CHECK(std::isfinite(camera.camera().position.y));
    CHECK(std::isfinite(camera.camera().position.z));
    camera.update({0, 48, 0}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    const auto before = camera.camera();
    camera.update({std::numeric_limits<f32>::quiet_NaN(), 0, 0}, 1.0f / 30);
    camera.update({0, 0, 0}, std::numeric_limits<f32>::infinity());
    CHECK(camera.camera() == before);
    CHECK_THROWS_AS(camera.reset({}, {0, 0, 0}), FormatError);
}

TEST_CASE("Legends camera native ticks and render samples are independent of render rate",
          "[legends][camera]") {
    LegendsCamera thirty;
    LegendsCamera sixty;
    LegendsCamera oneTwenty;
    thirty.reset(tower(), {0, 0, -20});
    sixty.reset(tower(), {0, 0, -20});
    oneTwenty.reset(tower(), {0, 0, -20});
    for (s32 tick = 0; tick < 180; ++tick) {
        const f32 rotation = static_cast<f32>(tick) * .01f;
        const Vec3 feet{std::sin(rotation) * 25, 0, std::cos(rotation) * 25};
        thirty.update(feet, 1.0f / 30);
        for (s32 i = 0; i < 2; ++i) {
            sixty.update(feet, 1.0f / 60);
        }
        for (s32 i = 0; i < 4; ++i) {
            oneTwenty.update(feet, 1.0f / 120);
        }
        CHECK(sixty.camera() == thirty.camera());
        CHECK(oneTwenty.camera() == thirty.camera());
        const auto native = sixty.camera();
        const auto half = sixty.sample(1.0f / 60);
        CHECK(std::isfinite(half.yaw));
        CHECK(sixty.camera() == native);
        CHECK(sixty.sample(1.0f / 30) == native);
    }
}

TEST_CASE("Legends entry pans from marker zero with native pre-roll and phase timing",
          "[legends][camera][camera-entry]") {
    auto data = markers();
    data.markers[0] = {{20.75f, 10.75f, -40.75f}, 2, 1};
    data.markers[1] = {{0, 0, 0}, 0, 0};
    LegendsCamera camera;
    camera.reset(data, {0, 0, 0});
    REQUIRE(camera.introActive());
    CHECK(camera.marker() == 1);

    // e00339a0 truncates opening XYZ; e007cdf0 sets peak distance /1.35.
    // Three .12-second steps give .36^2 / (2*.5*1.35) = .096 of the path.
    const Vec3 expectedPosition =
        glm::mix(Vec3{20, 10, -40}, Vec3{0, 3, 0}, .096f) - Vec3{0, 0, 36};
    const Vec3 expectedFocus = glm::mix(Vec3{0}, Vec3{0, 3, 0}, .096f);
    CHECK(glm::distance(camera.camera().position, expectedPosition) < .0001f);
    CHECK(glm::distance(camera.attention(), expectedFocus) < .0001f);
    CHECK(glm::dot(camera.camera().forward(), glm::normalize(expectedFocus - expectedPosition)) ==
          Approx(1));
    CHECK(camera.camera().yaw != data.markers.front().yaw);

    const auto initial = camera.camera();
    settle(camera, {0, 0, 0}, 40);
    CHECK(camera.introActive()); // .36 + 40/30 < .5 + 1 + .2
    CHECK(camera.camera().position != initial.position);
    const auto lastPan = camera.camera();
    camera.update({0, 0, 0}, 1.0f / 30);
    CHECK_FALSE(camera.introActive());
    CHECK(camera.camera().position == Vec3{0, 3, -36});
    CHECK(camera.camera().pitch == 0);
    CHECK(camera.camera().yaw == 0);
    CHECK(glm::distance(lastPan.position, camera.camera().position) < .01f);
    const auto handoff = camera.camera();
    camera.update({0, 0, 0}, 1.0f / 30);
    CHECK(camera.camera() == handoff);
}

TEST_CASE("Legends opening focus retargets without replaying on render samples",
          "[legends][camera][camera-entry]") {
    auto data = markers();
    data.markers[0].position = {-30, 20, -50};
    LegendsCamera thirty;
    LegendsCamera sixty;
    LegendsCamera oneTwenty;
    thirty.reset(data, {0, 0, 0});
    sixty.reset(data, {0, 0, 0});
    oneTwenty.reset(data, {0, 0, 0});
    for (s32 tick = 0; tick < 120; ++tick) {
        const Vec3 feet = tick < 10 ? Vec3{0} : Vec3{3, 1, 2};
        thirty.update(feet, 1.0f / 30);
        for (s32 frame = 0; frame < 2; ++frame) {
            sixty.update(feet, 1.0f / 60);
        }
        for (s32 frame = 0; frame < 4; ++frame) {
            oneTwenty.update(feet, 1.0f / 120);
        }
        CHECK(sixty.camera() == thirty.camera());
        CHECK(oneTwenty.camera() == thirty.camera());
        CHECK(sixty.introActive() == thirty.introActive());
        CHECK(oneTwenty.introActive() == thirty.introActive());
        const auto before = sixty.camera();
        for (s32 frame = 0; frame < 8; ++frame) {
            const auto sample = sixty.sample(static_cast<f32>(frame) / 240);
            CHECK(std::isfinite(sample.position.x));
            CHECK(std::isfinite(sample.pitch));
        }
        CHECK(sixty.camera() == before);
    }
    CHECK_FALSE(thirty.introActive());
    const auto before = thirty.camera();
    thirty.update({std::numeric_limits<f32>::quiet_NaN(), 0, 0}, 1.0f / 30);
    thirty.update({0, 0, 0}, std::numeric_limits<f32>::infinity());
    CHECK(thirty.camera() == before);
    thirty.reset(data, {0, 0, 0});
    CHECK(thirty.introActive());
    thirty.reset(tower(), {0, 0, -20});
    CHECK_FALSE(thirty.introActive());
}

TEST_CASE("Native tower entrance and realm arms produce finite moving cameras",
          "[legends][camera][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Native N64 extraction is not installed");
    }
    auto data = formats::decodeLegendsN64Camera(readFile(root / "levels/tower2/ist.rom"), "tower2",
                                                readFile(root / "_program/game.bin"));
    LegendsCamera camera;
    camera.reset(data, {-4.5f, 0, -15.5f}); // tower2's authored IPR entrance
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.camera().pitch == Approx(.05f));
    CHECK(camera.distance() == 55);
    for (const auto& exit : data.towerExits) {
        INFO("Exit " << exit.x << "," << exit.y << "," << exit.z);
        for (s32 step = 0; step < 300; ++step) {
            const Vec3 feet = exit * (static_cast<f32>(step) / 299);
            camera.update(feet, 1.0f / 30);
            const auto render = camera.sample(1.0f / 60);
            CHECK(std::isfinite(render.position.x));
            CHECK(std::isfinite(render.position.y));
            CHECK(std::isfinite(render.position.z));
            CHECK(glm::distance(camera.camera().position, camera.attention()) ==
                  Approx(camera.distance()).margin(.001f));
        }
        CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
        settle(camera, exit);
        CHECK(camera.camera().pitch == Approx(.7f));
    }
}
TEST_CASE("Native boss camera tracks a moving focus and removes it on defeat",
          "[legends][camera][bosses]") {
    auto data = markers();
    data.bossFocus = Vec3{0, 20, 0};
    LegendsCamera camera;
    camera.reset(data, Vec3{0}, false);
    const Vec3 focus{10, 30, 4};
    camera.trackBoss(focus, 12);
    settle(camera, Vec3{0}, 600);
    CHECK(glm::distance(camera.attention(), focus * .5f + camera.camera().up() * 3.0f) < .001f);
    camera.trackBoss(std::nullopt, 12);
    settle(camera, Vec3{0}, 600);
    CHECK(glm::distance(camera.attention(), camera.camera().up() * 3.0f) < .001f);
    CHECK(camera.distance() == 50); // Arena framing persists after the focus disappears.

    LegendsCamera ordinary;
    ordinary.reset(markers(), Vec3{0}, false);
    const auto unchanged = ordinary.camera();
    ordinary.trackBoss(focus, 12);
    settle(ordinary, Vec3{0}, 600);
    CHECK(ordinary.camera() == unchanged);
}

TEST_CASE("Native Mountain Valley opens with its authored pan then follows the player",
          "[legends][camera][camera-entry][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto data =
        formats::decodeLegendsN64Camera(readFile(root / "levels/mount1/ist.rom"), "mount1");
    REQUIRE(data.markers.size() > 18);
    REQUIRE(data.markers.front().position == Vec3{-75.75f, 91, 104.75f});
    const Vec3 feet{-3.5f, 55.75f, 91}; // Native IPR type20 entrance.
    // e00339a0 selects angles through e0035018's integer XYZ-quarter
    // distance, not the first follow marker. The authored nearest marker18
    // is 1075 squared quarters away; marker1 is 8654 quarters away.
    REQUIRE(data.markers[18].position == Vec3{-7.25f, 56.5f, 98.25f});
    const Vec3 nearestDelta = (feet - data.markers[18].position) * 4.0f;
    REQUIRE(glm::dot(nearestDelta, nearestDelta) == 1075.0f);
    for (usize index = 1; index < data.markers.size(); ++index) {
        const Vec3 delta = (feet - data.markers[index].position) * 4.0f;
        CHECK(glm::dot(delta, delta) >= 1075.0f);
    }
    LegendsCamera camera;
    camera.reset(data, feet);
    REQUIRE(camera.introActive());
    REQUIRE(camera.marker() == 18);
    WorldCamera orbit;
    orbit.pitch = data.markers[18].pitch;
    orbit.yaw = data.markers[18].yaw;
    const Vec3 focus = feet + orbit.up() * 3.0f;
    const Vec3 initial = glm::mix(Vec3{-75, 91, 104}, focus, .096f) - orbit.forward() * 36.0f;
    CHECK(glm::distance(camera.camera().position, initial) < .001f);
    for (s32 tick = 0; tick < 41; ++tick) {
        camera.update(feet, 1.0f / 30);
        const auto sample = camera.sample(1.0f / 60);
        CHECK(std::isfinite(sample.position.x));
        CHECK(std::isfinite(sample.position.y));
        CHECK(std::isfinite(sample.position.z));
        CHECK(std::isfinite(sample.pitch));
        CHECK(std::isfinite(sample.yaw));
    }
    CHECK_FALSE(camera.introActive());
    CHECK(glm::distance(camera.attention(), focus) < .001f);
    CHECK(glm::distance(camera.camera().position, focus - orbit.forward() * 36.0f) < .001f);
    const auto handoff = camera.camera();
    settle(camera, feet, 30);
    CHECK(glm::distance(camera.camera().position, handoff.position) < .001f);
    settle(camera, feet + Vec3{2, 0, 2}, 120);
    CHECK(glm::distance(camera.attention(), focus + Vec3{2, 0, 2}) < .001f);
}

TEST_CASE("N64 camera reports the mean feet it last followed", "[legends][camera]") {
    LegendsCamera camera;
    const std::array party{CameraSubject{{1, 0, 2}, {1, 3, 2}, 1},
                           CameraSubject{{3, 0, 6}, {3, 3, 6}, 1}};
    camera.reset(tower(), party);
    CHECK(camera.partyFeet() == Vec3{2, 0, 4});
    camera.update(Vec3{5, 1, 5}, 1.0f / 30);
    CHECK(camera.partyFeet() == Vec3{5, 1, 5});
    camera.update(Vec3{std::numeric_limits<f32>::quiet_NaN(), 0, 0}, 1.0f / 30);
    CHECK(camera.partyFeet() == Vec3{5, 1, 5});
}

TEST_CASE("N64 camera channel crosses a move from rest in its phases' seconds",
          "[legends][camera][tower-ceremony]") {
    // From rest a move takes acceleration + cruise + braking: 2 s for the shots, 1.8 s for the
    // return, cruising at its length over (acceleration + braking) / 2 + cruise (e007cdf0).
    for (const auto& [times, updates, cruise] :
         {std::tuple{kShotTimes, 60, 5 / 1.5f}, std::tuple{kReturnTimes, 54, 5 / 1.4f}}) {
        CAPTURE(updates);
        LegendsCameraChannel channel;
        channel.set({1, 2, 3}, true, times);
        CHECK(channel.phase() == Phase::Done);
        const Vec3 goal{4, 6, 3};
        channel.set(goal, false, times);
        CHECK(channel.phase() == Phase::Accelerating);
        bool cruised = false;
        for (s32 update = 1; update <= updates; ++update) {
            CAPTURE(update);
            CHECK(channel.advance(kUpdate) == (update == updates));
            CHECK((channel.value() == goal) == (update == updates));
            if (channel.phase() == Phase::Cruising) {
                cruised = true;
                CHECK(channel.speed() == cruise);
            }
        }
        CHECK(cruised);
        CHECK(channel.speed() == 0);
    }
}

TEST_CASE("N64 camera channel keeps a moving speed when it plans again",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    channel.set({0, 0, 0}, true, kShotTimes);
    channel.set({10, 0, 0}, false, kShotTimes);
    for (s32 update = 0; update < 30; ++update) {
        channel.advance(kUpdate);
    }
    REQUIRE(channel.phase() == Phase::Cruising);
    const f32 speed = channel.speed();
    CHECK(speed == 10 / 1.5f);
    // e007cdf0 keeps +0x28: the new move starts at the old speed, not from rest.
    channel.set({10, 5, 0}, false, kShotTimes);
    CHECK(channel.speed() == speed);
    CHECK(channel.phase() == Phase::Accelerating);
    const Vec3 from = channel.value();
    channel.advance(kUpdate);
    CHECK(glm::distance(channel.value(), from) > speed * kUpdate * .9f);
    LegendsCameraChannel fresh;
    fresh.set(from, true, kShotTimes);
    fresh.set({10, 5, 0}, false, kShotTimes);
    fresh.advance(kUpdate);
    CHECK(glm::distance(fresh.value(), from) < speed * kUpdate * .1f);
}

TEST_CASE("N64 camera channel drops a move under a thousandth where it stands",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    channel.set({0, 0, 0}, true, kShotTimes);
    // Every component under .001: e007d074 keeps the value, and the goal falls back to it.
    channel.set({.0009f, .0009f, .0009f}, false, kShotTimes);
    CHECK(channel.goal() == Vec3{0});
    // Its length is over .001 all the same, so e007cdf0 plans on; e007d774's first update
    // ends the move unmoved.
    CHECK(channel.phase() == Phase::Accelerating);
    CHECK(channel.advance(kUpdate));
    CHECK(channel.value() == Vec3{0});
    CHECK(channel.phase() == Phase::Done);
    // Under .001 in length too, the move is done as it is planned, still where it stood.
    channel.set({.0005f, 0, 0}, false, kShotTimes);
    CHECK(channel.phase() == Phase::Done);
    CHECK(channel.value() == Vec3{0});
    // A component of .001 itself escapes the drop, and the length of .001 snaps to the goal.
    channel.set({.001f, 0, 0}, false, kShotTimes);
    CHECK(channel.phase() == Phase::Done);
    CHECK(channel.value() == Vec3{.001f, 0, 0});
    // Moving, a channel whose every remaining component falls under .001 ends where it is:
    // a short turn's last update is smaller than that.
    LegendsCameraChannel turn{true};
    turn.set({0, 3, 0}, true, kShotTimes);
    turn.set({0, -3, 0}, false, kShotTimes);
    CHECK(advancesToRest(turn) == 60);
    CHECK(turn.value().y != -3);
    CHECK(turn.value().y == Approx(-3).margin(.001f));
    CHECK(turn.goal() == turn.value());
}

TEST_CASE("N64 angular camera channel turns the short way within (-pi, pi]",
          "[legends][camera][tower-ceremony]") {
    // From 3 to -3 the remaining wraps to .28 across pi (e00149d8), not 6 the long way.
    LegendsCameraChannel channel{true};
    channel.set({0, 3, 0}, true, kShotTimes);
    channel.set({0, -3, 0}, false, kShotTimes);
    for (s32 update = 1; update <= 60; ++update) {
        CAPTURE(update);
        channel.advance(kUpdate);
        const f32 yaw = channel.value().y;
        CHECK(yaw > -kPi);
        CHECK(yaw <= kPi);
        CHECK(std::abs(yaw) > 2.9f);
    }
    // A half turn back, -pi, wraps to pi: e00149d8 keeps pi and lifts -pi.
    LegendsCameraChannel half{true};
    half.set({0, 0, 0}, true, kShotTimes);
    half.set({0, -kPi, 0}, false, kShotTimes);
    CHECK(half.goal().y == kPi);
    half.advance(kUpdate);
    CHECK(half.value().y > 0);
}

TEST_CASE("N64 camera channel that is off rests, keeps out of planning and starts where set",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    CHECK_FALSE(channel.on());
    CHECK(channel.advance(kUpdate));
    // A move on a channel that is off puts it there at once (the setters' flag-2 branch).
    channel.set({5, 0, 0}, false, kShotTimes);
    CHECK(channel.on());
    CHECK(channel.value() == Vec3{5, 0, 0});
    CHECK(channel.phase() == Phase::Done);
    // Turned off while moving it plans and moves no more, and reports rest.
    channel.set({10, 0, 0}, false, kShotTimes);
    channel.advance(kUpdate);
    const Vec3 stopped = channel.value();
    const f32 speed = channel.speed();
    channel.turnOff();
    channel.plan(kReturnTimes);
    CHECK(channel.phase() == Phase::Accelerating);
    CHECK(channel.advance(kUpdate));
    CHECK(channel.value() == stopped);
    CHECK(channel.speed() == speed);
    // A cut puts the value at the goal, at rest.
    channel.set({1, 1, 1}, true, kShotTimes);
    CHECK(channel.value() == Vec3{1, 1, 1});
    CHECK(channel.speed() == 0);
}

TEST_CASE("N64 camera rig keeps its angles as e0014d04 reads them in WorldCamera's convention",
          "[legends][camera][tower-ceremony]") {
    // e007d2c0 stores the orbit as (-pitch, yaw, 0); the rotation channel holds the same
    // angles with -roll. Their rows are WorldCamera's right, up and forward.
    for (const f32 pitch : {-1.1f, -.3f, 0.0f, kPi / 6, 1.2f}) {
        for (const f32 yaw : {-3.0f, -1.0f, 0.0f, 1.0f, 2.5f, kPi}) {
            for (const f32 roll : {0.0f, .4f}) {
                CAPTURE(pitch, yaw, roll);
                WorldCamera camera;
                camera.pitch = pitch;
                camera.yaw = yaw;
                camera.roll = roll;
                const auto rows = nativeRows(-pitch, yaw, -roll);
                CHECK(glm::distance(rows[0], camera.right()) < 1e-6f);
                CHECK(glm::distance(rows[1], camera.up()) < 1e-6f);
                CHECK(glm::distance(rows[2], camera.forward()) < 1e-6f);
            }
        }
    }
}

TEST_CASE("N64 camera rig builds its view as it steps, from the centre, orbit and attention",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    WorldCamera gameplay;
    gameplay.position = {20, 30, 40};
    gameplay.pitch = .4f;
    gameplay.yaw = .8f;
    // e007de28 leaves the centre at the eye and the rotation at the angles; nothing shows
    // before e007dd70 builds it.
    rig.settle(gameplay);
    CHECK(rig.view() == WorldCamera{});
    CHECK(rig.step(kUpdate));
    CHECK(rig.view() == gameplay);
    CHECK_FALSE(rig.attention());
    // The orbit puts the eye back from the centre along its pitch and yaw (e007db00); the
    // rotation keeps the turn.
    rig.setOrbit(30, kPi / 6, kPi / 3, true);
    CHECK(rig.view() == gameplay);
    CHECK(rig.step(0));
    WorldCamera along;
    along.pitch = kPi / 6;
    along.yaw = kPi / 3;
    const Vec3 eye = gameplay.position - along.forward() * 30.0f;
    CHECK(rig.view().position == eye);
    CHECK(rig.view().pitch == gameplay.pitch);
    CHECK(rig.view().yaw == gameplay.yaw);
    // A point to look at turns the view to it.
    const Vec3 point{1, 2, 3};
    rig.setAttention(point, true);
    CHECK(rig.step(0));
    CHECK(rig.view().position == eye);
    CHECK(glm::distance(rig.view().forward(), glm::normalize(point - eye)) < 1e-5f);
    CHECK(rig.view().roll == 0);
    CHECK(rig.attention() == point);
    // No point gives the turn back to the rotation, and no rotation either leaves e00a929c's
    // identity.
    rig.setAttention(std::nullopt, true);
    rig.step(0);
    CHECK(rig.view().pitch == gameplay.pitch);
    CHECK(rig.view().yaw == gameplay.yaw);
    rig.setRotation(std::nullopt, true);
    rig.step(0);
    CHECK(rig.view().pitch == 0);
    CHECK(rig.view().yaw == 0);
    CHECK(rig.view().roll == 0);
    CHECK(rig.view().position == eye);
}

TEST_CASE("N64 camera rig plans its moving channels again on new times",
          "[legends][camera][tower-ceremony]") {
    // e007cfd0(0, ...) plans every channel that is on with the new times; a move under way
    // keeps its speed and takes the new phases from there.
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    rig.settle(WorldCamera{});
    rig.setAttention(Vec3{0, 0, 10}, true);
    rig.step(0);
    rig.setAttention(Vec3{0, 0, 20}, false);
    for (s32 update = 0; update < 10; ++update) {
        CHECK_FALSE(rig.step(kUpdate));
    }
    rig.setTimes(kReturnTimes);
    s32 updates = 0;
    while (updates < 200 && !rig.step(kUpdate)) {
        ++updates;
    }
    CHECK(updates + 1 == 54);
}

TEST_CASE("N64 camera rig returns along its orbit to the gameplay eye (e007df20)",
          "[legends][camera][tower-ceremony]") {
    // Sumner's shot at the origin with the party at (3, 0, 12), then case 0x28's return. The
    // values are e007cdf0's, e007d774's and e007db00's arithmetic worked through in float32:
    // the eye arcs back about the party, and the rotation's last remainders fall under .001
    // an update early, leaving its angles a fraction of a milliradian short, as retail's are.
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    WorldCamera gameplay;
    gameplay.position = {20, 30, 40};
    gameplay.pitch = .4f;
    gameplay.yaw = .8f;
    rig.settle(gameplay);
    const Vec3 party{3, 0, 12};
    rig.setCentre(party * .5f, true);
    rig.setOrbit(30, std::bit_cast<f32>(0x3f060a92U), std::bit_cast<f32>(0x3f860a92U), true);
    rig.setAttention(Vec3{0, 3, 0}, true);
    rig.setRotation(std::nullopt, true);
    rig.step(0);
    const auto shot = rig.view();
    rig.setTimes(kReturnTimes);
    rig.setCentre(party, false);
    rig.returnTo(gameplay.position, gameplay.pitch, gameplay.yaw);
    CHECK_FALSE(rig.attention());
    CHECK(rig.view() == shot);
    for (s32 update = 1; update <= 54; ++update) {
        CAPTURE(update);
        CHECK(rig.step(kUpdate) == (update == 54));
        const auto view = rig.view();
        if (update == 1) {
            // The rotation takes over at the look the shot showed.
            CHECK(glm::distance(view.position, Vec3{-21.0323219f, 15.0102158f, -6.94209099f}) <
                  1e-4f);
            CHECK(view.pitch == Approx(.4967421f).margin(1e-5f));
            CHECK(view.yaw == Approx(1.2491014f).margin(1e-5f));
            CHECK(view.pitch == Approx(shot.pitch).margin(1e-3f));
            CHECK(view.yaw == Approx(shot.yaw).margin(1e-3f));
        }
    }
    const auto end = rig.view();
    CHECK(glm::distance(end.position, Vec3{19.9999962f, 29.9999981f, 40.0000076f}) < 1e-4f);
    CHECK(end.pitch == Approx(.400128186f).margin(1e-5f));
    CHECK(end.yaw == Approx(.800594449f).margin(1e-5f));
    CHECK(end.roll == 0);
    CHECK(end.pitch != gameplay.pitch);
}
} // namespace
