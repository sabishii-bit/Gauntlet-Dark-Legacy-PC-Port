#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/menu/MenuInput.h"

namespace {

using namespace gdl;
using gdl::game::MenuBindings;
using gdl::game::MenuInput;
using gdl::game::readMenuInput;

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
    pad.buttons[static_cast<usize>(PadButton::B)] = true;
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

} // namespace
