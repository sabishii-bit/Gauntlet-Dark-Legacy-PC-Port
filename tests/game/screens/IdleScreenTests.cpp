#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/IdleScreen.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("screensaver waits ten minutes and excludes ineligible scenes", "[game][idle]") {
    IdleWatch watch;
    const Input input;
    REQUIRE_FALSE(watch.update(599.0, input, true));
    REQUIRE(watch.update(1.0, input, true));
    REQUIRE_FALSE(watch.update(900.0, input, false));
    REQUIRE_FALSE(watch.update(599.0, input, true));
    REQUIRE(watch.update(1.0, input, true));
}

TEST_CASE("screensaver consumes dismissal until all controls are released", "[game][idle]") {
    IdleWatch watch;
    Input input;
    REQUIRE(watch.update(600.0, input, true));
    input.beginPoll();
    input.setKey(Key::Space, true);
    REQUIRE_FALSE(watch.update(0.016, input, true));
    REQUIRE(watch.consumingInput());
    REQUIRE_FALSE(watch.update(600.0, input, true));
    REQUIRE(watch.consumingInput());
    input.beginPoll();
    input.setKey(Key::Space, false);
    REQUIRE_FALSE(watch.update(0.016, input, true));
    REQUIRE_FALSE(watch.consumingInput());
    REQUIRE_FALSE(watch.update(599.0, input, true));
}

TEST_CASE("any controller resets inactivity but analogue noise does not", "[game][idle]") {
    IdleWatch watch;
    Input input;
    PadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.1f;
    input.setPad(3, pad);
    REQUIRE_FALSE(watch.update(599.0, input, true));
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = -0.1f;
    input.setPad(3, pad);
    REQUIRE(watch.update(1.0, input, true));
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(3, pad);
    REQUIRE_FALSE(watch.update(0.016, input, true));
    REQUIRE(watch.consumingInput());
    watch.reset();
    REQUIRE_FALSE(watch.update(599.0, input, true));
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.9f;
    input.setPad(3, pad);
    REQUIRE_FALSE(watch.update(1.0, input, true));
    REQUIRE_FALSE(watch.update(599.0, input, true));
}

TEST_CASE("weapon flight is staggered and independent of render rate", "[game][idle]") {
    SaverMotion whole;
    SaverMotion frames;
    whole.start();
    frames.start();
    for (usize i = 1; i < SaverMotion::kCount; ++i) {
        REQUIRE(whole.weapons()[i].delay > whole.weapons()[i - 1].delay);
    }
    whole.update(5.0, glm::radians(60.0f), 4.0f / 3.0f);
    for (s32 i = 0; i < 300; ++i) {
        frames.update(1.0 / 60.0, glm::radians(60.0f), 4.0f / 3.0f);
    }
    for (usize i = 0; i < SaverMotion::kCount; ++i) {
        REQUIRE(whole.weapons()[i].visible);
        REQUIRE(whole.weapons()[i].position == frames.weapons()[i].position);
        REQUIRE(whole.weapons()[i].angle == frames.weapons()[i].angle);
        REQUIRE(glm::length(whole.weapons()[i].velocity) == Approx(1.0f));
    }
    for (s32 frame = 0; frame < 3600; ++frame) {
        whole.update(1.0 / 60.0, glm::radians(60.0f), 4.0f / 3.0f);
        for (const auto& weapon : whole.weapons()) {
            REQUIRE(std::isfinite(weapon.position.x));
            REQUIRE(std::abs(weapon.position.z) < 41.0f);
        }
    }
}

TEST_CASE("screensaver draws the four authored weapon trees and their fire",
          "[game][idle][unpacked]") {
    const auto root = test::unpackedOrSkip("POWERUPS/animations.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    IdleScreen screen;
    REQUIRE(screen.open(device, root));
    for (s32 frame = 0; frame < 300; ++frame) {
        screen.update(1.0 / 60.0, glm::radians(60.0f), 4.0f / 3.0f);
    }
    REQUIRE(screen.effects().count() == 4);
    for (usize i = 0; i < screen.effects().count(); ++i) {
        REQUIRE(screen.effects().effect(i).particles.field().size() > 0);
        REQUIRE(screen.effects().effect(i).particles.field().particleCount() > 0);
        const auto& field = screen.effects().effect(i).particles.field();
        for (usize e = 0; e < field.size(); ++e) {
            const auto& emitter = field.emitter(e);
            for (const auto& particle : emitter.particles()) {
                const Vec3 position = emitter.positionOf(particle);
                REQUIRE(std::isfinite(position.x));
                REQUIRE(std::isfinite(position.y));
                REQUIRE(std::isfinite(position.z));
            }
        }
    }
    screen.render(device, Mat4{1.0f}, 640, 480, glm::radians(60.0f));
    REQUIRE_FALSE(device.draws.empty());
    screen.close();
    REQUIRE_FALSE(screen.isOpen());
    REQUIRE(screen.effects().count() == 0);
    REQUIRE_FALSE(screen.open(device, test::scratchDirectory("idle-empty")));
}
} // namespace
