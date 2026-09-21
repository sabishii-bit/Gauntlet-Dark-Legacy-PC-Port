#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"
#include "game/players/PlayerControls.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("keys walk along the axes and combine on the diagonal", "[game][players][controls]") {
    Input input;
    input.beginPoll();
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, true, kNoPad).any());
    input.setKey(Key::W, true);
    MoveInput move = readMoveInput(input, PlayBindings{}, true, kNoPad);
    REQUIRE(move.any());
    REQUIRE(move.direction == Vec2{0.0f, 1.0f});
    REQUIRE(move.magnitude == 1.0f);

    input.setKey(Key::D, true);
    move = readMoveInput(input, PlayBindings{}, true, kNoPad);
    REQUIRE(move.direction.x == Approx(0.7071f));
    REQUIRE(move.direction.y == Approx(0.7071f));
    REQUIRE(move.magnitude == 1.0f);

    // Opposite keys cancel; a player without the keyboard feels none of it.
    input.setKey(Key::S, true);
    REQUIRE(readMoveInput(input, PlayBindings{}, true, kNoPad).direction == Vec2{1.0f, 0.0f});
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, kNoPad).any());
}

TEST_CASE("the potion buttons are held and the selector's are presses",
          "[game][players][controls]") {
    Input input;
    input.beginPoll();
    input.setKey(Key::E, true);
    input.setKey(Key::I, true);
    PlayButtons buttons = readPlayButtons(input, PlayBindings{}, true, kNoPad);
    REQUIRE(buttons.usePotion);
    REQUIRE_FALSE(buttons.throwPotion);
    REQUIRE_FALSE(buttons.attack);
    REQUIRE(buttons.selectorUp);
    // Held into the next frame, the potion is still held; the selector's press is over.
    input.beginPoll();
    buttons = readPlayButtons(input, PlayBindings{}, true, kNoPad);
    REQUIRE(buttons.usePotion);
    REQUIRE_FALSE(buttons.selectorUp);
    // On a pad the directional buttons are the selector's and no longer walk.
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::DpadLeft)] = true;
    pad.buttons[static_cast<usize>(PadButton::X)] = true;
    input.setPad(0, pad);
    buttons = readPlayButtons(input, PlayBindings{}, false, 0);
    REQUIRE(buttons.selectorLeft);
    REQUIRE(buttons.throwPotion);
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 0).any());
}

TEST_CASE("the attack is held by its key or its pad button", "[game][players][controls]") {
    Input input;
    input.beginPoll();
    REQUIRE_FALSE(readAttackInput(input, PlayBindings{}, true, kNoPad));
    input.setKey(Key::Space, true);
    REQUIRE(readAttackInput(input, PlayBindings{}, true, kNoPad));
    REQUIRE_FALSE(readAttackInput(input, PlayBindings{}, false, kNoPad)); // not the keyboard's
    input.setKey(Key::Space, false);
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(1, pad);
    REQUIRE(readAttackInput(input, PlayBindings{}, false, 1));
    REQUIRE(readAttackInput(input, PlayBindings{}, false, kAllPads));
    REQUIRE_FALSE(readAttackInput(input, PlayBindings{}, false, 0));
    PlayBindings rebound;
    rebound.padAttack = {PadButton::B};
    REQUIRE_FALSE(readAttackInput(input, rebound, false, 1));
}

TEST_CASE("the stick moves past its dead zone and the pad buttons add to it",
          "[game][players][controls]") {
    Input input;
    input.beginPoll();
    PadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.1f;
    input.setPad(1, pad);
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 1).any());

    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 0.0f;
    pad.axes[static_cast<usize>(PadAxis::LeftY)] = -1.0f; // the pad's up is negative
    input.setPad(1, pad);
    MoveInput move = readMoveInput(input, PlayBindings{}, false, 1);
    REQUIRE(move.direction == Vec2{0.0f, 1.0f});
    REQUIRE(move.magnitude == 1.0f);

    pad.axes[static_cast<usize>(PadAxis::LeftY)] = -0.625f;
    input.setPad(1, pad);
    move = readMoveInput(input, PlayBindings{}, false, 1);
    REQUIRE(move.magnitude == Approx(0.5f)); // half way through the live range

    pad.axes[static_cast<usize>(PadAxis::LeftY)] = 0.0f;
    // A pad's directional buttons walk only when bound to; by default they are the selector's.
    pad.buttons[static_cast<usize>(PadButton::DpadRight)] = true;
    input.setPad(1, pad);
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 1).any());
    PlayBindings walking;
    walking.padRight = {PadButton::DpadRight};
    move = readMoveInput(input, walking, false, 1);
    REQUIRE(move.direction == Vec2{1.0f, 0.0f});
    pad.axes[static_cast<usize>(PadAxis::LeftX)] = 1.0f;
    input.setPad(1, pad);

    // Another pad, or none, does not see it; every pad does.
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, 0).any());
    REQUIRE_FALSE(readMoveInput(input, PlayBindings{}, false, kNoPad).any());
    REQUIRE(readMoveInput(input, PlayBindings{}, false, kAllPads).any());
}

TEST_CASE("turbo is held; the charge and the attack are known the frame they go down",
          "[game][players][controls]") {
    Input input;
    input.beginPoll();
    input.setKey(Key::LeftShift, true);
    input.setKey(Key::F, true);
    input.setKey(Key::R, true);
    input.setKey(Key::C, true);
    input.setKey(Key::LeftControl, true);
    PlayButtons buttons = readPlayButtons(input, PlayBindings{}, true, kNoPad);
    REQUIRE(buttons.strongAttack);
    REQUIRE(buttons.shieldPotion);
    REQUIRE(buttons.strafe);
    REQUIRE(buttons.turbo);
    REQUIRE(buttons.chargePressed);
    REQUIRE_FALSE(buttons.attackPressed);
    input.beginPoll();
    input.setKey(Key::Space, true);
    buttons = readPlayButtons(input, PlayBindings{}, true, kNoPad);
    REQUIRE(buttons.turbo);
    REQUIRE_FALSE(buttons.chargePressed); // still held, no longer new
    REQUIRE(buttons.attack);
    REQUIRE(buttons.attackPressed);
    input.beginPoll();
    buttons = readPlayButtons(input, PlayBindings{}, true, kNoPad);
    REQUIRE(buttons.attack);
    REQUIRE_FALSE(buttons.attackPressed);
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::RightBumper)] = true;
    pad.buttons[static_cast<usize>(PadButton::Y)] = true;
    input.setPad(0, pad);
    REQUIRE(readPlayButtons(input, PlayBindings{}, false, 0).turbo);
    REQUIRE(readPlayButtons(input, PlayBindings{}, false, 0).chargePressed);
}

} // namespace
