#include <array>
#include <cmath>
#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/FlightVelocity.h"
#include "game/config/GameConfig.h"
#include "game/console/NoclipMotion.h"
#include "game/players/PlayerControls.h"
#include "game/rollback/InputCommand.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;
constexpr s32 kSettleSteps = 300;    ///< long enough for a flight to settle at what is wished
constexpr s32 kEaseSteps = 60;       ///< a second of easing toward one wish
constexpr s32 kCubeSteps = 1000;     ///< long past settling at full length
constexpr f32 kStepMargin = 1.0e-6f; ///< how far a step may stray from its eased one near nothing

bool alongside(const Vec3& a, const Vec3& b) {
    return glm::length(a) > 1e-4f && glm::length(b) > 1e-4f &&
           glm::dot(glm::normalize(a), glm::normalize(b)) > 0.999f;
}

Vec3 levelled(const Vec3& axis) {
    return glm::normalize(Vec3{axis.x, 0.0f, axis.z});
}

TEST_CASE("noclip's wish follows the movement keys along the ground as the camera looks, Q and "
          "E up and down, and Shift slows it",
          "[dev-console][noclip]") {
    const PlayBindings bindings; // the arrows and W A S D
    // A camera looking along +x, pitched well down: the keys still go along the ground.
    const WorldCamera camera{.position = Vec3{0.0f, 10.0f, 0.0f}, .pitch = 0.6f, .yaw = kPi / 2};
    Input input;
    const auto wish = [&](std::initializer_list<Key> keys) {
        input.beginPoll();
        for (const Key key : keys) {
            input.setKey(key, true);
        }
        const NoclipMotion::Wish asked = NoclipMotion::wish(input, bindings, true, kNoPad, camera);
        for (const Key key : keys) {
            input.setKey(key, false);
        }
        return asked;
    };
    CHECK(wish({}).direction == Vec3{0.0f});
    CHECK_FALSE(wish({}).slow);
    const NoclipMotion::Wish forward = wish({Key::W});
    CHECK(alongside(forward.direction, levelled(camera.forward())));
    CHECK(forward.direction.y == Approx(0.0f));
    CHECK(glm::length(forward.direction) == Approx(1.0f));
    CHECK_FALSE(forward.slow);
    CHECK(alongside(wish({Key::S}).direction, -levelled(camera.forward())));
    CHECK(alongside(wish({Key::D}).direction, levelled(camera.right())));
    CHECK(alongside(wish({Key::A}).direction, -levelled(camera.right())));
    CHECK(wish({Key::Q}).direction == Vec3{0.0f, 1.0f, 0.0f});
    CHECK(wish({Key::E}).direction == Vec3{0.0f, -1.0f, 0.0f});
    CHECK(wish({Key::W, Key::LeftShift}).slow);
    // Forward and up together share a unit's length.
    const NoclipMotion::Wish both = wish({Key::W, Key::Q});
    CHECK(glm::length(both.direction) == Approx(1.0f));
    CHECK(both.direction.y == Approx(std::sqrt(0.5f)).margin(1e-3f));
    // Without the keyboard, the keys count for nothing.
    input.beginPoll();
    input.setKey(Key::W, true);
    CHECK(NoclipMotion::wish(input, bindings, false, kNoPad, camera).direction == Vec3{0.0f});
}

TEST_CASE("noclip's wish follows the left stick along the ground and the right stick up and down",
          "[dev-console][noclip]") {
    const PlayBindings bindings;
    const WorldCamera camera{.position = Vec3{0.0f, 10.0f, 0.0f}, .pitch = 0.3f, .yaw = 0.0f};
    Input input;
    PadSnapshot pad;
    pad.connected = true;
    const auto wish = [&](f32 leftX, f32 leftY, f32 rightY, s32 which) {
        input.beginPoll();
        pad.axes[static_cast<usize>(PadAxis::LeftX)] = leftX;
        pad.axes[static_cast<usize>(PadAxis::LeftY)] = leftY;
        pad.axes[static_cast<usize>(PadAxis::RightY)] = rightY;
        input.setPad(0, pad);
        return NoclipMotion::wish(input, bindings, false, which, camera);
    };
    // The stick's y grows downwards: pushed up, the character goes the way the camera looks.
    CHECK(alongside(wish(0.0f, -1.0f, 0.0f, 0).direction, levelled(camera.forward())));
    CHECK(alongside(wish(1.0f, 0.0f, 0.0f, 0).direction, levelled(camera.right())));
    CHECK(wish(0.0f, -1.0f, 0.0f, 0).direction.y == Approx(0.0f));
    // The right stick up rises, down sinks, inside its dead zone neither.
    CHECK(wish(0.0f, 0.0f, -1.0f, 0).direction == Vec3{0.0f, 1.0f, 0.0f});
    CHECK(wish(0.0f, 0.0f, 1.0f, 0).direction == Vec3{0.0f, -1.0f, 0.0f});
    CHECK(wish(0.0f, 0.0f, 0.1f, 0).direction == Vec3{0.0f});
    CHECK(wish(0.0f, 0.0f, -0.6f, 0).direction.y ==
          Approx((0.6f - NoclipMotion::kRiseDeadZone) / (1.0f - NoclipMotion::kRiseDeadZone)));
    // Every pad, or no pad.
    CHECK(wish(0.0f, 0.0f, -1.0f, kAllPads).direction == Vec3{0.0f, 1.0f, 0.0f});
    CHECK(wish(0.0f, 0.0f, -1.0f, kNoPad).direction == Vec3{0.0f});
    CHECK_FALSE(wish(0.0f, 0.0f, -1.0f, 0).slow); // Shift is the keyboard's alone
}

TEST_CASE("noclip's flight eases toward the wish, settles slower with Shift and stops at rest",
          "[dev-console][noclip]") {
    NoclipMotion flight;
    CHECK(flight.velocity() == Vec3{0.0f});
    NoclipMotion::Wish wish{.direction = Vec3{0.0f, 0.0f, 1.0f}};
    // It starts slow and comes up to the whole wish.
    Vec3 flown = flight.fly(wish, kStep);
    CHECK(flown.z > 0.0f);
    CHECK(flown.z < 1.0f);
    for (s32 i = 0; i < kSettleSteps; ++i) {
        flown = flight.fly(wish, kStep);
    }
    CHECK(flown.z == Approx(1.0f).epsilon(0.01));
    // Slowly, it settles at a quarter of the pace; at rest, it stops.
    wish.slow = true;
    for (s32 i = 0; i < kSettleSteps; ++i) {
        flown = flight.fly(wish, kStep);
    }
    CHECK(glm::length(flown) == Approx(NoclipMotion::kSlowFactor).epsilon(0.01));
    for (s32 i = 0; i < kSettleSteps; ++i) {
        flown = flight.fly(NoclipMotion::Wish{}, kStep);
    }
    CHECK(flown == Vec3{0.0f});
    CHECK(flight.velocity() == Vec3{0.0f});
    // A reset stops it at once.
    flight.fly(wish, kStep);
    REQUIRE(flight.velocity() != Vec3{0.0f});
    flight.reset();
    CHECK(flight.velocity() == Vec3{0.0f});
    CHECK(flight.fly(NoclipMotion::Wish{}, kStep) == Vec3{0.0f});
}

TEST_CASE("the eased velocity flown as a wish moves as the eased step", "[dev-console][noclip]") {
    // The scene moves a flown character by the wish times kSpeed and the tick's seconds, which
    // is the step a velocity easing toward the wish at kSpeed takes.
    NoclipMotion flight;
    FlightVelocity eased;
    const std::array wishes{
        NoclipMotion::Wish{.direction = glm::normalize(Vec3{0.6f, 0.3f, -0.8f})},
        NoclipMotion::Wish{.direction = Vec3{-1.0f, 0.0f, 0.0f}, .slow = true},
        NoclipMotion::Wish{},
    };
    for (const NoclipMotion::Wish& wish : wishes) {
        CAPTURE(wish.direction.x, wish.direction.y, wish.direction.z, wish.slow);
        const f32 pace = NoclipMotion::kSpeed * (wish.slow ? NoclipMotion::kSlowFactor : 1.0f);
        for (s32 i = 0; i < kEaseSteps; ++i) {
            const Vec3 flown = flight.fly(wish, kStep);
            const Vec3 step = eased.advance(wish.direction * pace, kStep);
            CHECK(flight.velocity() == eased.velocity);
            const Vec3 moved = flown * NoclipMotion::kSpeed * kStep;
            CHECK(moved.x == Approx(step.x).margin(kStepMargin));
            CHECK(moved.y == Approx(step.y).margin(kStepMargin));
            CHECK(moved.z == Approx(step.z).margin(kStepMargin));
        }
    }
}

TEST_CASE("the wish never leaves the unit cube", "[dev-console][noclip][console-input]") {
    // Flown at full length along each axis for a long while, every wish is one the input
    // protocol takes.
    for (const Vec3& axis :
         {Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
          Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}}) {
        CAPTURE(axis.x, axis.y, axis.z);
        NoclipMotion flight;
        InputCommand command{.epoch = 1, .grant = 1};
        bool taken = true;
        for (s32 i = 0; i < kCubeSteps; ++i) {
            command.consoleFlight = flight.fly(NoclipMotion::Wish{.direction = axis}, kStep);
            taken = taken && command.valid();
        }
        CHECK(taken);
        CHECK(glm::dot(command.consoleFlight, axis) == Approx(1.0f));
    }
}

} // namespace
