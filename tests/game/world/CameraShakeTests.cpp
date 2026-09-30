#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/CameraShake.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
TEST_CASE("shake requests retain priority, startup delay, radius and affected endpoints",
          "[game][camera][shake]") {
    CameraShake shake;
    const WorldCamera camera;
    const Vec3 attention{0, 0, 10};
    shake.start(CameraShake::Target::Both, 4, 30, 0.3f, 200);
    REQUIRE(shake.offset() == Vec3{0});
    shake.start(); // the lower-priority hit must not replace the turbo request
    shake.update(4);
    REQUIRE(glm::length(shake.offset()) == Approx(0.3f));
    const auto both = shake.apply(camera, attention);
    REQUIRE(both.position == camera.position + shake.offset());
    REQUIRE(both.yaw == Approx(camera.yaw));
    REQUIRE(both.pitch == Approx(camera.pitch));
    shake.start(CameraShake::Target::Eye, 0, 10, 0.5f, 200);
    const auto eye = shake.apply(camera, attention);
    REQUIRE(eye.position == camera.position + shake.offset());
    REQUIRE(eye.yaw != camera.yaw);
    shake.update(11);
    REQUIRE_FALSE(shake.active());
    shake.start();
    REQUIRE(glm::length(shake.offset()) == Approx(0.1f));
    shake.clear();
    REQUIRE_FALSE(shake.active());
}
TEST_CASE("combat shake orbits attention without displacing or drifting the camera",
          "[game][yeti][camera]") {
    CameraShake shake;
    const WorldCamera original;
    const Vec3 attention{0, 0, 10};
    REQUIRE(shake.apply(original, attention).yaw == 0);
    shake.start();
    REQUIRE(glm::length(shake.offset()) == Approx(0.1f));
    REQUIRE(shake.offset().y == 0);
    shake.update(2);
    const auto first = shake.apply(original, attention);
    REQUIRE(first.position == original.position);
    REQUIRE(first.yaw != original.yaw);
    REQUIRE(shake.apply(original, attention).yaw == first.yaw); // draw is not a clock
    shake.update(88);
    REQUIRE(shake.active());
    shake.update(1);
    REQUIRE_FALSE(shake.active());
    REQUIRE(shake.apply(original, attention).yaw == original.yaw);
    shake.start();
    shake.clear();
    REQUIRE_FALSE(shake.active());
}
} // namespace
