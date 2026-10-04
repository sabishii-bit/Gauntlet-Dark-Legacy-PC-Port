
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/menu/MenuInput.h"

namespace {

using namespace gdl;
using gdl::game::MenuBindings;
using gdl::game::MenuInput;
using gdl::game::MenuInputSource;
using gdl::game::readMenuInput;

TEST_CASE("pointer regions isolate player lanes without taking away device commands",
          "[game][menu][mouse][multiplayer]") {
    for (const auto extent : std::array{Vec2{640, 448}, Vec2{1920, 1080}, Vec2{900, 1200}}) {
        const Mat4 transform = makeLetterboxProjection(512, 384, extent.x, extent.y);
        for (s32 owner = 0; owner < 4; ++owner) {
            const Vec4 point =
                transform * Vec4{static_cast<f32>(owner) * 128.0f + 64, 180, 0.5f, 1};
            Input raw;
            raw.setPointer({(point.x / point.w + 1) / 2, (point.y / point.w + 1) / 2, true, true});
            raw.latchPointerBack();
            raw.scrollPointer(-2.5f);
            for (s32 lane = 0; lane < 4; ++lane) {
                const auto input = readMenuInput(raw, {}, MenuInputSource::forPlayer(lane));
                const Rect region{static_cast<f32>(lane) * 128.0f, 0, 128, 384};
                const auto mapped = gdl::game::mapMenuPointer(input, transform, region);
                CHECK(mapped.pointer.has_value() == (lane == owner));
                CHECK(mapped.pointerPressed == (lane == owner));
                CHECK(mapped.pointerHeld == (lane == owner));
                CHECK(mapped.pointerBack == (lane == owner));
                CHECK(mapped.back == (lane == owner));
                CHECK(mapped.pointerScroll == (lane == owner ? -2.5f : 0));
                MenuInput device = input;
                device.back = true;
                device.down = true;
                const auto simultaneous = gdl::game::mapMenuPointer(device, transform, region);
                CHECK(simultaneous.back);
                CHECK(simultaneous.down);
            }
        }
    }
    MenuInput boundary;
    boundary.pointer = Vec2{128, 20};
    boundary.pointerPressed = true;
    CHECK_FALSE(gdl::game::mapMenuPointer(boundary, Mat4{1}, Rect{0, 0, 128, 384}).pointer);
    CHECK(gdl::game::mapMenuPointer(boundary, Mat4{1}, Rect{128, 0, 128, 384}).pointer);
    Input outside;
    outside.scrollPointer(1);
    CHECK(readMenuInput(outside, {}).pointerScroll == 0);
}

TEST_CASE("mouse taps survive render polls and map through letterboxed canvas coordinates",
          "[game][menu][mouse]") {
    Input polled;
    Input simulation;
    polled.beginPoll();
    polled.setPointer({0.5f, 0.5f, true, false});
    polled.latchPointer();
    polled.latchPointerBack();
    simulation.accumulate(polled);
    polled.beginPoll();
    simulation.accumulate(polled);
    const auto menu = readMenuInput(simulation, MenuBindings{}, MenuInputSource::forPlayer(2));
    REQUIRE(menu.pointer);
    CHECK(menu.pointerPressed);
    CHECK_FALSE(menu.pointerHeld);
    const auto transform = makeLetterboxProjection(512, 384, 1920, 1080);
    const auto mapped = gdl::game::mapMenuPointer(menu, transform);
    REQUIRE(mapped.pointer);
    CHECK(std::abs(mapped.pointer->x - 256) < 0.001f);
    CHECK(std::abs(mapped.pointer->y - 192) < 0.001f);
    CHECK(mapped.back);
    simulation.beginPoll();
    CHECK_FALSE(readMenuInput(simulation, MenuBindings{}).pointerPressed);
    polled.setPointer({0, 0.5f, true, false});
    const auto margin = gdl::game::mapMenuPointer(readMenuInput(polled, MenuBindings{}), transform);
    REQUIRE(margin.pointer);
    CHECK(margin.pointer->x < 0);
    polled.setPointer({0, 0, false, false});
    CHECK_FALSE(readMenuInput(polled, MenuBindings{}).pointer);
}

TEST_CASE("keyboard presses map to menu commands", "[game][menu]") {
    Input input;
    input.beginPoll();
    REQUIRE_FALSE(readMenuInput(input, MenuBindings{}).any());

    input.setKey(Key::Enter, true);
    MenuInput menu = readMenuInput(input, MenuBindings{});
    REQUIRE(menu.select);
    REQUIRE(menu.start);
    REQUIRE_FALSE(menu.back);

    input.beginPoll();
    input.setKey(Key::Down, true);
    input.setKey(Key::Backspace, true);
    menu = readMenuInput(input, MenuBindings{});
    REQUIRE(menu.down);
    REQUIRE(menu.back);
    REQUIRE_FALSE(menu.select);

    input.beginPoll();
    menu = readMenuInput(input, MenuBindings{});
    REQUIRE_FALSE(menu.any());
}

TEST_CASE("pad buttons map to menu commands", "[game][menu]") {
    Input input;
    input.beginPoll();
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::DpadUp)] = true;
    pad.buttons[static_cast<usize>(PadButton::Y)] = true;
    input.setPad(2, pad);
    const MenuInput menu = readMenuInput(input, MenuBindings{});
    REQUIRE(menu.up);
    REQUIRE(menu.back);
    REQUIRE_FALSE(menu.start);

    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::Start)] = true;
    input.setPad(2, pad);
    const MenuInput next = readMenuInput(input, MenuBindings{});
    REQUIRE(next.start);
    REQUIRE_FALSE(next.up);
}

TEST_CASE("bindings decide which keys and buttons count", "[game][menu]") {
    MenuBindings bindings;
    bindings.select = {Key::X};
    bindings.start = {};
    bindings.padSelect = {PadButton::Y};
    Input input;
    input.beginPoll();
    input.setKey(Key::Enter, true);
    REQUIRE_FALSE(readMenuInput(input, bindings).any());

    input.beginPoll();
    input.setKey(Key::X, true);
    REQUIRE(readMenuInput(input, bindings).select);

    input.beginPoll();
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::Y)] = true;
    input.setPad(0, pad);
    REQUIRE(readMenuInput(input, bindings).select);
}

TEST_CASE("held directions are reported alongside presses", "[game][menu]") {
    Input input;
    input.beginPoll();
    input.setKey(Key::Up, true);
    MenuInput menu = readMenuInput(input, MenuBindings{});
    REQUIRE(menu.up);
    REQUIRE(menu.upHeld);
    input.beginPoll();
    menu = readMenuInput(input, MenuBindings{});
    REQUIRE_FALSE(menu.up);
    REQUIRE(menu.upHeld);
    REQUIRE_FALSE(menu.downHeld);
    input.beginPoll();
    input.setKey(Key::Up, false);
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::DpadLeft)] = true;
    input.setPad(1, pad);
    menu = readMenuInput(input, MenuBindings{}, MenuInputSource::forPlayer(1));
    REQUIRE(menu.left);
    REQUIRE(menu.leftHeld);
    REQUIRE_FALSE(menu.upHeld);
    REQUIRE_FALSE(readMenuInput(input, MenuBindings{}, MenuInputSource::forPlayer(0)).leftHeld);
}

TEST_CASE("four controllers steer only their own menu lanes with held sticks",
          "[game][menu][multiplayer]") {
    Input input;
    const std::array axes{PadAxis::LeftY, PadAxis::LeftX, PadAxis::LeftY, PadAxis::LeftX};
    const std::array directions{-0.8f, 0.8f, 0.8f, -0.8f};
    std::array<PadSnapshot, Input::kMaxPads> pads;
    for (s32 player = 0; player < Input::kMaxPads; ++player) {
        auto& pad = pads[static_cast<usize>(player)];
        pad.connected = true;
        pad.axes[static_cast<usize>(axes[static_cast<usize>(player)])] =
            directions[static_cast<usize>(player)];
        input.setPad(player, pad);
    }
    for (s32 player = 0; player < Input::kMaxPads; ++player) {
        const auto menu = readMenuInput(input, {}, MenuInputSource::forPlayer(player));
        CHECK(menu.up == (player == 0));
        CHECK(menu.right == (player == 1));
        CHECK(menu.down == (player == 2));
        CHECK(menu.left == (player == 3));
    }
    input.beginPoll();
    for (s32 player = 0; player < Input::kMaxPads; ++player) {
        const auto menu = readMenuInput(input, {}, MenuInputSource::forPlayer(player));
        CHECK_FALSE(menu.any());
        CHECK(menu.upHeld == (player == 0));
        CHECK(menu.rightHeld == (player == 1));
        CHECK(menu.downHeld == (player == 2));
        CHECK(menu.leftHeld == (player == 3));
    }
    // A disconnected pad cannot continue steering; the remaining slots stay assigned.
    input.setPad(1, {});
    CHECK_FALSE(readMenuInput(input, {}, MenuInputSource::forPlayer(1)).rightHeld);
    CHECK(readMenuInput(input, {}, MenuInputSource::forPlayer(3)).leftHeld);
    input.beginPoll();
    input.setPad(1, pads[1]);
    CHECK(readMenuInput(input, {}, MenuInputSource::forPlayer(1)).right);
    CHECK_FALSE(readMenuInput(input, {}, MenuInputSource::forPlayer(3)).left);
    MenuBindings rebound;
    rebound.padRight = {PadButton::DpadRight};
    CHECK_FALSE(readMenuInput(input, rebound, MenuInputSource::forPlayer(1)).right);
}

TEST_CASE("the escape binding is read from the keyboard alone, typing or not", "[game][menu]") {
    Input input;
    input.beginPoll();
    input.setKey(Key::Escape, true);
    REQUIRE(readMenuInput(input, MenuBindings{}).escape);
    REQUIRE(readMenuInput(input, MenuBindings{}, MenuInputSource{}.typing()).escape);
    REQUIRE_FALSE(readMenuInput(input, MenuBindings{}, MenuInputSource::forPlayer(1)).escape);
    MenuBindings bindings;
    bindings.escape = {Key::Q};
    REQUIRE_FALSE(readMenuInput(input, bindings).escape);
    input.beginPoll();
    input.setKey(Key::Escape, false);
    REQUIRE_FALSE(readMenuInput(input, MenuBindings{}).escape);
}

TEST_CASE("a text field takes the typing keys away from the menu", "[game][menu]") {
    Input input;
    input.beginPoll();
    input.setKey(Key::W, true);
    input.setKey(Key::Space, true);
    input.setKey(Key::Backspace, true);
    input.addTypedChar('w');
    input.addTypedChar(' ');
    input.addTypedChar(0x20AC); // outside ASCII: dropped
    MenuInput menu = readMenuInput(input, MenuBindings{});
    REQUIRE(menu.up);
    REQUIRE(menu.select);
    REQUIRE(menu.back);
    REQUIRE(menu.typed.empty());
    REQUIRE_FALSE(menu.erase);

    menu = readMenuInput(input, MenuBindings{}, MenuInputSource{}.typing());
    REQUIRE_FALSE(menu.up);
    REQUIRE_FALSE(menu.upHeld);
    REQUIRE_FALSE(menu.select);
    REQUIRE_FALSE(menu.back);
    REQUIRE(menu.typed == "w ");
    REQUIRE(menu.erase);

    // Arrows and Enter still steer while typing.
    input.beginPoll();
    input.setKey(Key::W, false);
    input.setKey(Key::Space, false);
    input.setKey(Key::Backspace, false);
    input.setKey(Key::Up, true);
    input.setKey(Key::Enter, true);
    menu = readMenuInput(input, MenuBindings{}, MenuInputSource::forPlayer(0).typing());
    REQUIRE(menu.up);
    REQUIRE(menu.select);
    REQUIRE(menu.typed.empty());
    REQUIRE_FALSE(menu.erase);

    // Without the keyboard nothing is typed.
    input.addTypedChar('x');
    menu = readMenuInput(input, MenuBindings{}, MenuInputSource::forPlayer(1).typing());
    REQUIRE(menu.typed.empty());
}

} // namespace
