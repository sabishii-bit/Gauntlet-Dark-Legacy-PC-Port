#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace {

using namespace gdl;

bool near(f32 a, f32 b, f32 tolerance = 1e-4f) {
    return std::abs(a - b) <= tolerance;
}

bool near(const Vec3& a, const Vec3& b, f32 tolerance = 1e-4f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

TEST_CASE("billboards retain animated size while replacing orientation", "[world][camera]") {
    const CameraFrame camera = CameraFrame::at({5, 10, -20});
    for (const u32 mode : {1U, CameraFrame::kFacingFull}) {
        for (const Vec3 scale : {Vec3{0.001f}, Vec3{3, 7, 2}, Vec3{0}}) {
            const Mat4 model = glm::scale(glm::translate(Mat4{1}, Vec3{1, 2, 3}), scale);
            const Mat4 faced = camera.face(model, mode);
            REQUIRE(faced[3] == model[3]);
            for (s32 axis = 0; axis < 3; ++axis) {
                REQUIRE(near(glm::length(Vec3{faced[axis]}), scale[axis]));
            }
        }
    }
}

TEST_CASE("an unturned camera looks along positive z in a left-handed frame", "[world][camera]") {
    const WorldCamera camera;
    REQUIRE(near(camera.forward(), Vec3{0.0f, 0.0f, 1.0f}));
    REQUIRE(near(camera.up(), Vec3{0.0f, 1.0f, 0.0f}));
    REQUIRE(near(camera.right(), Vec3{1.0f, 0.0f, 0.0f}));
    REQUIRE(near(glm::cross(camera.right(), camera.up()), camera.forward()));
    const Vec4 ahead = camera.view() * Vec4{2.0f, 3.0f, 10.0f, 1.0f};
    REQUIRE(near(ahead.x, 2.0f));
    REQUIRE(near(ahead.y, 3.0f));
    REQUIRE(near(ahead.z, 10.0f));
}

TEST_CASE("top-facing lightning ribbons retain their authored axis and face the eye",
          "[world][camera][legend]") {
    // CameraFace 800C7EB0 dispatches mode 8 to TopFaceMat 800B95EC:
    // x = normalize(cross(eye-position,z)), y = cross(z,x), z is untouched.
    const auto camera = CameraFrame::at({5, 10, -20});
    for (const f32 scale : {0.001f, 1.0f, 3.0f, -0.005f}) {
        Mat4 model = glm::rotate(glm::translate(Mat4{1}, Vec3{1, 2, 3}), 0.7f,
                                 glm::normalize(Vec3{1, 2, 3}));
        model = glm::scale(model, Vec3{scale});
        const Mat4 faced = camera.face(model, CameraFrame::kFacingTop);
        CHECK(faced[2] == model[2]);
        CHECK(faced[3] == model[3]);
        const auto side =
            glm::normalize(glm::cross(camera.position - Vec3{model[3]}, Vec3{model[2]}));
        CHECK(near(Vec3{faced[0]}, side));
        CHECK(near(Vec3{faced[1]}, glm::cross(Vec3{model[2]}, side)));
        CHECK(glm::dot(Vec3{faced[1]}, camera.position - Vec3{model[3]}) > 0);
    }
    CHECK(CameraFrame::facingOf(0x08001800) == CameraFrame::kFacingTop);
}

TEST_CASE("top-facing ribbons have a finite fallback for axial eyes and collapsed keys",
          "[world][camera][legend]") {
    const auto camera = CameraFrame::at({0, 0, 10});
    const Mat4 axial = camera.face(Mat4{1}, CameraFrame::kFacingTop);
    CHECK(axial == Mat4{1});
    const Mat4 collapsed = glm::scale(Mat4{1}, Vec3{0});
    const Mat4 faced = camera.face(collapsed, CameraFrame::kFacingTop);
    CHECK(faced[0] == Vec4{0});
    CHECK(faced[1] == Vec4{0, 1, 0, 0});
    CHECK(faced[2] == collapsed[2]);
    CHECK(faced[3] == collapsed[3]);
}

TEST_CASE("yaw turns about y and a positive pitch looks down", "[world][camera]") {
    WorldCamera camera;
    camera.yaw = kPi;
    REQUIRE(near(camera.forward(), Vec3{0.0f, 0.0f, -1.0f}));
    REQUIRE(near(camera.right(), Vec3{-1.0f, 0.0f, 0.0f}));
    camera.yaw = kHalfPi;
    REQUIRE(near(camera.forward(), Vec3{1.0f, 0.0f, 0.0f}));

    WorldCamera tilted;
    tilted.pitch = 0.5f;
    REQUIRE(near(tilted.forward(), Vec3{0.0f, -std::sin(0.5f), std::cos(0.5f)}));
    REQUIRE(near(tilted.right(), Vec3{1.0f, 0.0f, 0.0f}));
    REQUIRE(near(glm::dot(tilted.forward(), tilted.up()), 0.0f));
    REQUIRE(tilted.up().y > 0.0f);

    // The tower's entrance camera sits above the floor and sees the start point below it.
    WorldCamera entrance;
    entrance.position = Vec3{3.59f, 20.52f, -9.23f};
    entrance.pitch = 0.551f;
    entrance.yaw = 3.138f;
    const Vec3 toStart = glm::normalize(Vec3{3.19f, -0.09f, -31.78f} - entrance.position);
    REQUIRE(glm::dot(toStart, entrance.forward()) > 0.95f);

    WorldCamera rolled;
    rolled.roll = kHalfPi;
    REQUIRE(near(rolled.forward(), Vec3{0.0f, 0.0f, 1.0f}));
    REQUIRE(near(rolled.right(), Vec3{0.0f, 1.0f, 0.0f}));
    REQUIRE(near(rolled.up(), Vec3{-1.0f, 0.0f, 0.0f}));
}

TEST_CASE("the view moves the world in front of the eye", "[world][camera]") {
    WorldCamera camera;
    camera.position = Vec3{5.0f, 1.0f, -20.0f};
    camera.yaw = kPi;
    const Vec4 ahead = camera.view() * Vec4{5.0f, 1.0f, -30.0f, 1.0f};
    REQUIRE(near(ahead.x, 0.0f));
    REQUIRE(near(ahead.y, 0.0f));
    REQUIRE(near(ahead.z, 10.0f));
    const Vec4 left = camera.view() * Vec4{6.0f, 1.0f, -30.0f, 1.0f};
    REQUIRE(near(left.x, -1.0f)); // facing -z, world +x is to the eye's left
}

TEST_CASE("the projection keeps depth reversed and under the 2D layers", "[world][camera]") {
    const Mat4 projection = WorldCamera::projection(glm::radians(60.0f), 640.0f / 448.0f);
    const auto depthAt = [&](f32 distance) {
        const Vec4 clip = projection * Vec4{0.0f, 0.0f, distance, 1.0f};
        return clip.z / clip.w;
    };
    REQUIRE(near(depthAt(WorldCamera::kNear), WorldCamera::kDepthRange, 1e-3f));
    REQUIRE(near(depthAt(WorldCamera::kFar), 0.0f, 1e-3f));
    REQUIRE(depthAt(10.0f) > depthAt(100.0f));
    REQUIRE(depthAt(10.0f) < WorldCamera::kDepthRange);
    // A point at the edge of the horizontal field of view lands on the clip edge.
    const f32 edge = std::tan(glm::radians(30.0f)) * 10.0f;
    const Vec4 side = projection * Vec4{edge, 0.0f, 10.0f, 1.0f};
    REQUIRE(near(side.x / side.w, 1.0f, 1e-3f));
    const Vec4 above = projection * Vec4{0.0f, 1.0f, 10.0f, 1.0f};
    REQUIRE(above.y / above.w > 0.0f);
}

TEST_CASE("clip space maps onto the letterboxed frame", "[world][camera]") {
    const Mat4 mapping = WorldCamera::frameMapping(640.0f, 448.0f);
    const Vec4 corner = mapping * Vec4{-1.0f, 1.0f, 0.3f, 1.0f};
    REQUIRE(near(corner.x, 0.0f));
    REQUIRE(near(corner.y, 0.0f));
    REQUIRE(near(corner.z, 0.3f));
    const Vec4 opposite = mapping * Vec4{1.0f, -1.0f, 0.3f, 1.0f};
    REQUIRE(near(opposite.x, 640.0f));
    REQUIRE(near(opposite.y, 448.0f));

    WorldCamera camera;
    camera.position = Vec3{0.0f, 0.0f, -5.0f};
    const Mat4 clip = camera.clipTransform(glm::radians(60.0f), 640.0f, 448.0f,
                                           makeScreenProjection(640.0f, 448.0f));
    const Vec4 centre = clip * Vec4{0.0f, 0.0f, 5.0f, 1.0f};
    REQUIRE(near(centre.x / centre.w, 0.0f, 1e-3f));
    REQUIRE(near(centre.y / centre.w, 0.0f, 1e-3f));
    REQUIRE(centre.z / centre.w > 0.0f);
    REQUIRE(centre.z / centre.w < WorldCamera::kDepthRange);
    // Something above the eye line ends up in the upper half of the frame.
    const Vec4 high = clip * Vec4{0.0f, 2.0f, 5.0f, 1.0f};
    REQUIRE(high.y / high.w < 0.0f);
}

} // namespace
