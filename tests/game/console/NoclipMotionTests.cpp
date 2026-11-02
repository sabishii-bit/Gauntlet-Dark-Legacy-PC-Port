#include <cmath>
#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/world/WorldCamera.h"

#include "game/config/GameConfig.h"
#include "game/console/NoclipMotion.h"
#include "game/players/PlayerControls.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;

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

TEST_CASE("noclip eases toward the wish and faces the way it goes along the ground",
          "[dev-console][noclip]") {
    NoclipMotion flight;
    CHECK(flight.velocity() == Vec3{0.0f});
    NoclipMotion::Wish wish{.direction = Vec3{0.0f, 0.0f, 1.0f}};
    NoclipMotion::Step step = flight.advance(wish, kStep);
    CHECK(step.offset.z > 0.0f);
    CHECK(step.offset.z < NoclipMotion::kSpeed * kStep);
    REQUIRE(step.yaw.has_value());
    CHECK(*step.yaw == Approx(0.0f));
    for (s32 i = 0; i < 300; ++i) {
        step = flight.advance(wish, kStep);
    }
    CHECK(step.offset.z == Approx(NoclipMotion::kSpeed * kStep).epsilon(0.01));
    // Along +x it faces a quarter turn; straight up it keeps its facing.
    wish.direction = Vec3{1.0f, 0.0f, 0.0f};
    CHECK(flight.advance(wish, kStep).yaw == Approx(kPi / 2));
    wish.direction = Vec3{0.0f, 1.0f, 0.0f};
    CHECK_FALSE(flight.advance(wish, kStep).yaw.has_value());
    // Slowly, it settles at a quarter of the pace; at rest, it stops.
    wish.slow = true;
    for (s32 i = 0; i < 300; ++i) {
        step = flight.advance(wish, kStep);
    }
    CHECK(glm::length(step.offset) ==
          Approx(NoclipMotion::kSpeed * NoclipMotion::kSlowFactor * kStep).epsilon(0.01));
    flight.reset();
    CHECK(flight.velocity() == Vec3{0.0f});
    CHECK(flight.advance(NoclipMotion::Wish{}, kStep).offset == Vec3{0.0f});
}

} // namespace
