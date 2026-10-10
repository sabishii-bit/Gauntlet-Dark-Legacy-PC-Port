#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("every network action maps independently to the existing gameplay input",
          "[netplay][controls]") {
    InputCommand command;
    command.epoch = 1;
    command.grant = 1;
    command.direction = {0.6f, 0.8f};
    command.magnitude = 0.75f;
    command.aimPoint = Vec3{7, 8, 9};
    for (u32 bit = 1; bit <= InputCommand::kPressMask; bit <<= 1U) {
        command.pressedButtons = bit;
        const auto play = SessionInputs::playInput(command);
        const auto result = SessionInputs::command(play, 1, 0, 1, 0);
        CHECK(result.pressedButtons == bit);
        CHECK(result.heldButtons == 0);
        CHECK(result.direction == command.direction);
        CHECK(result.magnitude == command.magnitude);
        CHECK(result.aimPoint == command.aimPoint);
    }
    command.pressedButtons = 0;
    for (u32 bit = 1; bit <= InputCommand::kHeldMask; bit <<= 1U) {
        command.heldButtons = bit;
        const auto result = SessionInputs::command(SessionInputs::playInput(command), 1, 0, 1, 0);
        CHECK(result.heldButtons == bit);
        CHECK(result.pressedButtons == 0);
    }
}

TEST_CASE("offline input retains local pointer text and device capture without quantization",
          "[netplay][controls][multiplayer]") {
    const Input devices;
    SessionInputs session;
    SessionInputs::Frame local;
    for (auto& input : local) {
        input.move = {{0.6f, 0.8f}, 0.1234567f};
        input.attack = true;
        input.attackPressed = true;
        input.selector.up = true;
        input.menu.devices = &devices;
        input.menu.typed = "TEST";
        input.menu.pointer = Vec2{0.125f, 0.75f};
        input.menu.pointerScroll = -1;
        input.menu.pointerNormalized = true;
        input.menu.pointerPressed = true;
        input.menu.pointerHeld = true;
        input.menu.pointerBack = true;
        input.menu.erase = true;
    }
    for (const auto& result : session.advance(local)) {
        CHECK(result.move.direction == local[0].move.direction);
        CHECK(result.move.magnitude == local[0].move.magnitude);
        CHECK(result.attack);
        CHECK(result.attackPressed);
        CHECK(result.selector.up);
        CHECK(result.menu.devices == &devices);
        CHECK(result.menu.typed == "TEST");
        CHECK(result.menu.pointer == local[0].menu.pointer);
        CHECK(result.menu.pointerScroll == -1);
        CHECK(result.menu.pointerNormalized);
        CHECK(result.menu.pointerPressed);
        CHECK(result.menu.pointerHeld);
        CHECK(result.menu.pointerBack);
        CHECK(result.menu.erase);
        CHECK_FALSE(result.menu.buttonPressed);
    }
    CHECK(session.timeline().tick() == 1);
    for (const auto& result : session.advance({})) {
        CHECK_FALSE(result.attack);
        CHECK_FALSE(result.attackPressed);
        CHECK_FALSE(result.selector.up);
    }
}

TEST_CASE("remote seats ignore this machine's devices and cannot retain UI pointers",
          "[netplay][controls][multiplayer]") {
    SessionInputs session;
    auto& timeline = session.timeline();
    REQUIRE(timeline.assign(2, 2));
    REQUIRE(timeline.assign(3, 0));
    SessionInputs::Frame local;
    for (auto& input : local) {
        input.attack = true;
        input.move = {{1, 0}, 1};
    }
    PlayInput remote;
    const Input devices;
    remote.menu.devices = &devices;
    remote.menu.typed = "DO NOT SEND";
    remote.menu.pointer = Vec2{1};
    remote.menu.pointerPressed = true;
    remote.movieSkipPressed = true;
    remote.move = {{-1, 0}, 1};
    remote.throwPotion = true;
    const auto command =
        SessionInputs::command(remote, timeline.epoch(), timeline.tick(), timeline.grant(2), 2);
    REQUIRE(timeline.submit(2, command) == InputTimeline::Admission::Accepted);
    const auto result = session.advance(local);
    CHECK(result[0].attack);
    CHECK(result[1].attack);
    CHECK_FALSE(result[2].attack);
    CHECK(result[2].move.direction == Vec2{-1, 0});
    CHECK(result[2].throwPotion);
    CHECK(result[2].menu.buttonPressed);
    CHECK(result[2].menu.devices == nullptr);
    CHECK_FALSE(result[2].menu.pointer);
    CHECK(result[2].menu.typed.empty());
    CHECK_FALSE(result[2].movieSkipPressed);
    CHECK_FALSE(result[3].attack);
    CHECK(result[3].move.magnitude == 0);
    CHECK_FALSE(session.advance(local)[2].throwPotion);
}
} // namespace
