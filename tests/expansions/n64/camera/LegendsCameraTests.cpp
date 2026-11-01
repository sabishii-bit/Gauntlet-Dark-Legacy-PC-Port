#include <cmath>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/camera/LegendsCamera.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

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
} // namespace
