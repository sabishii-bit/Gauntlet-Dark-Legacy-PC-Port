#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/MenuInput.h"

namespace {

using namespace gdl;
using gdl::game::MenuInput;
using gdl::game::readMenuInput;

TEST_CASE("keyboard presses map to menu commands", "[game][menu]") {
    Input input;
    input.beginPoll();
    REQUIRE_FALSE(readMenuInput(input).any());

    input.setKey(Key::Enter, true);
    MenuInput menu = readMenuInput(input);
    REQUIRE(menu.select);
    REQUIRE(menu.start);
    REQUIRE_FALSE(menu.back);

    input.beginPoll();
    input.setKey(Key::Down, true);
    input.setKey(Key::Backspace, true);
    menu = readMenuInput(input);
    REQUIRE(menu.down);
    REQUIRE(menu.back);
    REQUIRE_FALSE(menu.select);

    input.beginPoll();
    menu = readMenuInput(input);
    REQUIRE_FALSE(menu.any());
}

TEST_CASE("pad buttons map to menu commands", "[game][menu]") {
    Input input;
    input.beginPoll();
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::DpadUp)] = true;
    pad.buttons[static_cast<usize>(PadButton::B)] = true;
    input.setPad(2, pad);
    const MenuInput menu = readMenuInput(input);
    REQUIRE(menu.up);
    REQUIRE(menu.back);
    REQUIRE_FALSE(menu.start);

    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::Start)] = true;
    input.setPad(2, pad);
    const MenuInput next = readMenuInput(input);
    REQUIRE(next.start);
    REQUIRE_FALSE(next.up);
}

} // namespace
