#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCollision.h"

#include "expansions/dreamcast/camera/LegendsDreamcastCamera.h"

namespace gdl::game {
namespace {
CameraSubject subject(Vec3 feet, f32 height = 3) {
    return {feet, feet + Vec3{0, height, 0}, height};
}

void near(Vec3 actual, Vec3 expected, f32 tolerance = .001f) {
    CHECK(glm::distance(actual, expected) < tolerance);
}

TEST_CASE("Dreamcast tower uses its native radial hub instead of a shoulder camera",
          "[legends][dreamcast-camera]") {
    const std::array party{subject({0, 0, 32})};
    LegendsDreamcastTowerCamera camera;
    camera.reset(party, {0, 3, 0});
    near(camera.camera().position, {0, 17, 0});
    near(camera.attention(), {0, 11, 32});
    CHECK(camera.camera().yaw == Catch::Approx(0));
    CHECK(camera.camera().pitch == Catch::Approx(std::atan2(6.0f, 32.0f)));
    CHECK(glm::distance(camera.camera().position, camera.attention()) > 32);
    CHECK(LegendsDreamcastTowerCamera::kHorizontalFov == Catch::Approx(80 * kPi / 180));

    const auto initial = camera.camera();
    const auto attention = camera.attention();
    camera.reset(std::array{subject({0, 0, 32}, 12)}, {0, 3, 0});
    CHECK(camera.camera() == initial);
    CHECK(camera.attention() == attention);
    camera.reset(std::array{subject({32, 0, 0})}, {0, 3, 0});
    near(camera.camera().position, {0, 17, 0});
    CHECK(camera.camera().yaw == Catch::Approx(kPi / 2));
    camera.reset(std::array{subject({-32, 0, 0})}, {0, 3, 0});
    CHECK(camera.camera().yaw == Catch::Approx(-kPi / 2));
}

TEST_CASE("Dreamcast tower follows outer rooms from the hub side of their walls",
          "[legends][dreamcast-camera]") {
    const Vec3 hub{0, 3.07764f, -10.6521f};
    const Vec3 feet{58, 7, -7};
    LegendsDreamcastTowerCamera camera;
    camera.reset(std::array{subject(feet)}, hub);
    const auto view = camera.camera();
    CHECK(view.position.x > hub.x);
    CHECK(view.position.x < feet.x);
    CHECK(view.position.y > feet.y);
    CHECK(glm::dot(view.forward(), glm::normalize(camera.attention() - view.position)) > .999f);

    WorldCollision walls;
    walls.build({{{-1, 0, 0}, {Vec3{60, -50, -100}, Vec3{60, 100, -100}, Vec3{60, 100, 100}}},
                 {{-1, 0, 0}, {Vec3{60, -50, -100}, Vec3{60, 100, 100}, Vec3{60, -50, 100}}}});
    CHECK_FALSE(walls.pickSurface(camera.attention(), view.position));
}

TEST_CASE("Dreamcast tower weights the whole party and limits its native target count",
          "[legends][dreamcast-camera]") {
    std::array party{subject({0, 0, 20}), subject({0, 0, 60})};
    LegendsDreamcastTowerCamera camera;
    camera.reset(party, {0, 3, 0});
    CHECK(camera.attention().z ==
          Catch::Approx((20.0f / 20 + 60.0f * 10 / 60) / (1.0f / 20 + 10.0f / 60)));
    // The nearer player determines the native radial eye height, while both
    // players still contribute to the weighted attention point.
    CHECK(camera.camera().position.y == Catch::Approx(3 + (20 - 12) * .7f));
    CHECK(camera.attention().y == Catch::Approx(3 + (20 - 12) * .45f - 1));
    const auto expected = camera.camera();
    std::swap(party[0], party[1]);
    camera.reset(party, {0, 3, 0});
    near(camera.camera().position, expected.position);
    std::array many{subject({0, 0, 20}), subject({0, 0, 20}), subject({0, 0, 20}),
                    subject({0, 0, 20}), subject({1000, 0, 0})};
    camera.reset(std::span{many}.first(4), {0, 3, 0});
    const auto four = camera.camera();
    camera.reset(many, {0, 3, 0});
    CHECK(camera.camera() == four);
}

TEST_CASE("Dreamcast tower filtering is frame-rate independent and can be interpolated",
          "[legends][dreamcast-camera]") {
    const std::array start{subject({0, 0, 32})};
    const std::array moved{subject({10, 0, 32})};
    LegendsDreamcastTowerCamera at30;
    LegendsDreamcastTowerCamera at60;
    at30.reset(start, {0, 3, 0});
    at60.reset(start, {0, 3, 0});
    const auto previous = at30.camera();
    at30.update(moved, 1.0f / 30);
    at60.update(moved, 1.0f / 60);
    CHECK(at60.camera() == previous);
    at60.update(moved, 1.0f / 60);
    CHECK(at60.camera() == at30.camera());
    CHECK(at60.attention() == at30.attention());
    near(at30.camera().interpolate(previous, .5f).position,
         (at30.camera().position + previous.position) * .5f);
    for (int tick = 0; tick < 90; ++tick) {
        at30.update(moved, 1.0f / 30);
        at60.update(moved, 1.0f / 60);
        at60.update(moved, 1.0f / 60);
    }
    CHECK(at60.camera() == at30.camera());
}

TEST_CASE("Dreamcast tower camera handles empty parties and its radial singularity",
          "[legends][dreamcast-camera]") {
    LegendsDreamcastTowerCamera camera;
    camera.reset({}, {0, 3, 0});
    CHECK(std::isfinite(camera.camera().pitch));
    camera.update(std::array{subject({0, 0, 0})}, 1.0f / 30);
    CHECK(std::isfinite(camera.camera().pitch));
    CHECK(std::isfinite(camera.camera().yaw));
    const auto previous = camera.camera();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    camera.update(std::array{subject({nan, 0, 0})}, 1.0f / 30);
    camera.update({}, 1.0f / 30);
    camera.update(std::array{subject({5, 0, 0})}, std::numeric_limits<f32>::infinity());
    camera.update(std::array{subject({5, 0, 0})}, -1);
    CHECK(camera.camera() == previous);
}

TEST_CASE("Dreamcast tower rendering spans both halves of each native camera step",
          "[legends][dreamcast-camera]") {
    const std::array start{subject({0, 0, 32})};
    const std::array moved{subject({10, 0, 32})};
    LegendsDreamcastTowerCamera camera;
    LegendsDreamcastTowerCamera unsampled;
    camera.reset(start, {0, 3, 0});
    unsampled.reset(start, {0, 3, 0});
    const auto first = camera.camera();
    const auto firstAttention = camera.attention();
    for (int tick = 0; tick < 2; ++tick) {
        camera.update(moved, 1.0f / 60);
        unsampled.update(moved, 1.0f / 60);
    }
    const auto second = camera.camera();
    const auto secondAttention = camera.attention();
    REQUIRE(glm::distance(first.position, second.position) > .01f);
    near(camera.sample().position, first.position);
    near(camera.sampleAttention(), firstAttention);
    const auto firstQuarter = camera.sample(1.0f / 120);
    near(firstQuarter.position, glm::mix(first.position, second.position, .25f));
    near(camera.sampleAttention(1.0f / 120), glm::mix(firstAttention, secondAttention, .25f));

    // The following 60 Hz update has no new native pose. It still presents
    // the second half of the existing pair rather than holding the current eye.
    camera.update(moved, 1.0f / 60);
    unsampled.update(moved, 1.0f / 60);
    CHECK(camera.camera() == second);
    near(camera.sample().position, glm::mix(first.position, second.position, .5f));
    near(camera.sample(1.0f / 120).position, glm::mix(first.position, second.position, .75f));
    near(camera.sampleAttention(1.0f / 120), glm::mix(firstAttention, secondAttention, .75f));
    near(camera.sample(1.0f / 60).position, second.position);
    for (int frame = 0; frame < 100; ++frame) {
        const auto sampled = camera.sample(static_cast<f32>(frame) / 6000);
        CHECK(std::isfinite(sampled.pitch));
    }
    camera.update(moved, 1.0f / 60);
    unsampled.update(moved, 1.0f / 60);
    near(camera.sample().position, second.position);
    CHECK(camera.camera() == unsampled.camera());
    CHECK(camera.attention() == unsampled.attention());
    CHECK(camera.sample() == unsampled.sample());
}

TEST_CASE("Dreamcast tower blocks only inward movement near the native eye",
          "[legends][dreamcast-camera]") {
    const Vec3 hub{0, 3.07764f, -10.6521f};
    LegendsDreamcastTowerCamera camera;
    camera.reset(std::array{subject({0, 0, 8})}, hub);
    const Vec3 eye = camera.camera().position;
    for (const Vec3 radial : std::array{Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 1},
                                        Vec3{0, 0, -1}, glm::normalize(Vec3{1, 0, 1})}) {
        const Vec3 tangent{-radial.z, 0, radial.x};
        const Vec3 from{eye.x + radial.x * 12, 0, eye.z + radial.z * 12};
        near(camera.constrainMovement(from, from - radial), from);
        near(camera.constrainMovement(from, from + radial), from + radial);
        near(camera.constrainMovement(from, from + tangent), from + tangent);
        near(camera.constrainMovement(from, from - radial + tangent + Vec3{0, 2, 0}),
             from + tangent + Vec3{0, 2, 0});
        const Vec3 outside = from + radial;
        near(camera.constrainMovement(outside, outside - radial), outside - radial);
    }
    // Recovery from an old save or a console teleport must not normalize zero.
    const Vec3 center{eye.x, 0, eye.z};
    near(camera.constrainMovement(center, center + Vec3{1, 0, 0}), center + Vec3{1, 0, 0});
    const auto before = camera.constrainMovement(center + Vec3{12, 0, 0}, center + Vec3{11, 0, 1});
    for (s32 frame = 0; frame < 120; ++frame) {
        (void)camera.sample(static_cast<f32>(frame) / 3600);
        (void)camera.sampleAttention(static_cast<f32>(frame) / 3600);
    }
    CHECK(camera.constrainMovement(center + Vec3{12, 0, 0}, center + Vec3{11, 0, 1}) == before);
}

TEST_CASE("Dreamcast tower approach cannot cross through the eye and reverse its yaw",
          "[legends][dreamcast-camera]") {
    const Vec3 hub{0, 3.07764f, -10.6521f};
    for (const f32 bodyHeight : std::array{3.0f, 12.0f}) {
        for (const Vec3 radial :
             std::array{Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
            Vec3 feet{hub.x + radial.x * 20, 0, hub.z + radial.z * 20};
            LegendsDreamcastTowerCamera camera;
            camera.reset(std::array{subject(feet, bodyHeight)}, hub);
            const f32 expectedYaw = camera.camera().yaw;
            for (s32 tick = 0; tick < 180; ++tick) {
                feet = camera.constrainMovement(feet, feet - radial * .2f);
                camera.update(std::array{subject(feet, bodyHeight)}, 1.0f / 60);
                CHECK(std::isfinite(camera.camera().pitch));
                CHECK(camera.camera().yaw == Catch::Approx(expectedYaw));
                CHECK(glm::dot(Vec3{feet.x - hub.x, 0, feet.z - hub.z}, radial) >= 12.2f);
            }
        }
    }
}
TEST_CASE("Dreamcast entry uses its authored eye and native hold before panning",
          "[legends][dreamcast-camera]") {
    const std::array markers{
        WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 0, 0}, {.6f, kPi, 0}},
        WorldLocator{LocatorKind::CameraStart, 0, 0, {0, 30, 100}, {.2f, kPi, 0}}};
    const std::array party{subject({0, 0, 0})};
    LegendsDreamcastLevelCamera camera;
    camera.reset(party, markers, {20, 26, .4f});
    REQUIRE(camera.introActive());
    CHECK(camera.marker() == 0);
    near(camera.camera().position, markers[1].position);
    near(camera.attention(),
         markers[1].position + camera.camera().forward() * glm::length(markers[1].position));
    const auto start = camera.camera();
    for (s32 tick = 0; tick < 44; ++tick) {
        camera.update(party, 1.0f / 30);
        CHECK(camera.camera() == start);
        CHECK(camera.introActive());
    }
    camera.update(party, 1.0f / 30);
    CHECK(camera.camera() != start);
    CHECK(camera.introActive());
    for (s32 tick = 0; tick < 180 && camera.introActive(); ++tick) {
        camera.update(party, 1.0f / 30);
    }
    CHECK_FALSE(camera.introActive());
    near(camera.attention(), {0, 0, 0});
    CHECK(glm::distance(camera.camera().position, camera.attention()) == Catch::Approx(20));

    camera.reset(party, markers, {20, 26, .4f}, false);
    CHECK_FALSE(camera.introActive());
    CHECK(camera.camera() != start);
}

TEST_CASE("Dreamcast transmitters use feet height and do not follow the entrance locator",
          "[legends][dreamcast-camera]") {
    const std::array markers{WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 0, 0}, {.5f, kPi, 0}},
                             WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 30, 0}, {1.0f, 0, 0}},
                             WorldLocator{LocatorKind::CameraStart, 0, 0, {0, 0, 0}, {0, 0, 0}}};
    const CameraRange range{20, 26, .4f, {-50, 3, -50}, {50, 60, 50}};
    LegendsDreamcastLevelCamera normal;
    LegendsDreamcastLevelCamera grown;
    normal.reset(std::array{subject({0, 0, 0})}, markers, range, false);
    grown.reset(std::array{subject({0, 0, 0}, 30)}, markers, range, false);
    CHECK(normal.camera() == grown.camera());
    CHECK(normal.marker() == 0);
    near(normal.attention(), {0, 3, 0});
    for (s32 tick = 0; tick < 120; ++tick) {
        normal.update(std::array{subject({0, 30, 0})}, 1.0f / 30);
        grown.update(std::array{subject({0, 30, 0}, 30)}, 1.0f / 30);
        CHECK(normal.camera() == grown.camera());
    }
    CHECK(normal.marker() == 1);
    CHECK(normal.camera().pitch == Catch::Approx(1.0f));
}

TEST_CASE("Dreamcast native entrance and follow are independent of render cadence",
          "[legends][dreamcast-camera]") {
    const std::array markers{
        WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 0, 0}, {.6f, kPi, 0}},
        WorldLocator{LocatorKind::CameraStart, 0, 0, {0, 30, 100}, {.2f, kPi, 0}}};
    const std::array party{subject({0, 0, 0})};
    LegendsDreamcastLevelCamera at30;
    LegendsDreamcastLevelCamera at60;
    LegendsDreamcastLevelCamera at120;
    for (auto* camera : {&at30, &at60, &at120}) {
        camera->reset(party, markers, {20, 26, .4f});
    }
    for (s32 tick = 0; tick < 240; ++tick) {
        const auto before = at30.camera();
        const auto beforeAttention = at30.attention();
        const auto subjects =
            std::array{subject({tick < 150 ? 0.0f : static_cast<f32>(tick - 150) * .1f, 0, 0})};
        at30.update(subjects, 1.0f / 30);
        for (s32 half = 0; half < 2; ++half) {
            at60.update(subjects, 1.0f / 60);
        }
        for (s32 quarter = 0; quarter < 4; ++quarter) {
            at120.update(subjects, 1.0f / 120);
        }
        CHECK(at30.camera() == at60.camera());
        CHECK(at30.camera() == at120.camera());
        CHECK(at30.introActive() == at120.introActive());
        near(at30.sample(1.0f / 60).position, (before.position + at30.camera().position) * .5f);
        near(at30.sampleAttention(1.0f / 60), (beforeAttention + at30.attention()) * .5f);
    }
}

TEST_CASE("Dreamcast single-player follow easing does not mistake movement for screen occupancy",
          "[legends][dreamcast-camera]") {
    const std::array markers{WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 0, 0}, {.6f, kPi, 0}}};
    LegendsDreamcastLevelCamera camera;
    camera.reset(std::array{subject({0, 0, 0})}, markers, {20, 26, .4f}, false);
    // A large relocation must retain the native single-player .55 occupancy,
    // not select the slower high-occupancy easing because the actor moved fast.
    camera.update(std::array{subject({100, 0, 0})}, 1.0f / 30);
    near(camera.attention(), {100.0f * .6f / 16.0f, 0, 0});
    camera.update(std::array{subject({100, 0, 0})}, 1.0f / 30);
    near(camera.attention(), {3.75f + 96.25f * 1.2f / 16.0f, 0, 0});
}

TEST_CASE("Dreamcast normal boss ceremony retains the completed eye and freezes its subject",
          "[legends][dreamcast-camera]") {
    const std::array markers{WorldLocator{LocatorKind::CameraGame, 0, 0, {0, 0, 0}, {.6f, kPi, 0}}};
    LegendsDreamcastLevelCamera camera;
    camera.reset(std::array{subject({0, 0, 0})}, markers, {20, 40, .4f}, false);
    const Vec3 eye = camera.camera().position;
    const Vec3 wizard{10, 12.5f, 10};
    camera.focusOn(wizard);
    CHECK(camera.camera().position == eye);
    near(camera.attention(), wizard);
    CHECK(glm::dot(camera.camera().forward(), glm::normalize(wizard - eye)) > .999f);
    const auto held = camera.camera();
    for (s32 tick = 0; tick < 120; ++tick) {
        camera.update(std::array{subject({100, -100, 100})}, 1.0f / 30);
        CHECK(camera.camera() == held);
        CHECK(camera.sample(1.0f / 60) == held);
    }
    camera.reset(std::array{subject({0, 0, 0})}, markers, {20, 40, .4f}, false);
    near(camera.attention(), {0, 0, 0});
}
} // namespace
} // namespace gdl::game
