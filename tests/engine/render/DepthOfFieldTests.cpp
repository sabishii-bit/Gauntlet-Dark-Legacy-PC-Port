#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/render/DepthOfField.h"
#include "engine/world/WorldCamera.h"

using namespace gdl;

TEST_CASE("depth of field reconstructs camera distance with reversed Z and widescreen", "[dof]") {
    WorldCamera camera;
    camera.position = {3, 9, -7};
    camera.pitch = 0.4f;
    camera.yaw = -0.3f;
    for (const auto width : {640.0f, 960.0f}) {
        const auto clip = camera.clipTransform(degreesToRadians(60), width, 448,
                                               makeScreenProjection(width, 448));
        DepthOfField blur;
        blur.clipToView = camera.view() * glm::inverse(clip);
        for (const f32 distance : {1.0f, 20.0f, 100.0f, 1000.0f}) {
            const Vec4 world = glm::inverse(camera.view()) * Vec4{0, 0, distance, 1};
            const Vec4 projected = clip * world;
            const Vec3 ndc = Vec3{projected} / projected.w;
            CHECK(blur.viewDistance(Vec2{ndc} * 0.5f + Vec2{0.5f}, ndc.z) ==
                  Catch::Approx(distance).margin(0.02));
        }
    }
}

TEST_CASE("depth of field leaves the foreground sharp and smoothly blurs distant scenery",
          "[dof]") {
    DepthOfField blur;
    blur.focusEnd = 25;
    blur.transition = 50;
    CHECK(blur.blurFraction(0) == 0);
    CHECK(blur.blurFraction(25) == 0);
    CHECK(blur.blurFraction(50) == Catch::Approx(0.5));
    CHECK(blur.blurFraction(75) == 1);
    CHECK(blur.blurFraction(2000) == 1);
}
