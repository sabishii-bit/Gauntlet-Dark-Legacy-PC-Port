#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/CameraShake.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
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
