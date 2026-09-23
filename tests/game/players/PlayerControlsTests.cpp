
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
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
    REQUIRE(buttons.usePotion);
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
    pad.buttons[static_cast<usize>(PadButton::B)] = true;
    pad.axes[static_cast<usize>(PadAxis::LeftTrigger)] = 1.0f;
    input.setPad(0, pad);
    REQUIRE(readPlayButtons(input, PlayBindings{}, false, 0).turbo);
    REQUIRE(readPlayButtons(input, PlayBindings{}, false, 0).chargePressed);
}

TEST_CASE("GameCube action chords follow remapped bindings and consume their parts",
          "[game][players][controls]") {
    Input input;
    PlayBindings b;
    // Deliberately avoid the default face buttons: combinations refer to actions.
    b.padAttack = {PadButton::LeftBumper};
    b.padTurbo = {PadButton::RightBumper};
    b.padUsePotion = {PadButton::RightThumb};
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::RightBumper)] = true;
    input.setPad(0, pad);
    REQUIRE(readPlayButtons(input, b, false, 0).turbo);
    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::LeftBumper)] = true;
    input.setPad(0, pad);
    REQUIRE(readPlayButtons(input, b, false, 0).turboAttackPressed);
    input.beginPoll();
    REQUIRE_FALSE(readPlayButtons(input, b, false, 0).turboAttackPressed);
    pad.buttons[static_cast<usize>(PadButton::RightThumb)] = true;
    input.setPad(0, pad);
    auto buttons = readPlayButtons(input, b, false, 0);
    REQUIRE(buttons.shieldPotion); // Magic takes priority over turbo; shield over throw.
    REQUIRE_FALSE(buttons.usePotion);
    REQUIRE_FALSE(buttons.throwPotion);
    REQUIRE_FALSE(buttons.attack);
    REQUIRE_FALSE(buttons.turboAttackPressed);
    pad.buttons[static_cast<usize>(PadButton::RightBumper)] = false;
    input.setPad(0, pad);
    buttons = readPlayButtons(input, b, false, 0);
    REQUIRE(buttons.throwPotion);
    REQUIRE_FALSE(buttons.usePotion);
    b.actionChords = false;
    buttons = readPlayButtons(input, b, false, 0);
    REQUIRE(buttons.usePotion);
    REQUIRE(buttons.attack);
    REQUIRE_FALSE(buttons.throwPotion);
}

TEST_CASE("chords cannot combine different pads or a keyboard with a pad",
          "[game][players][controls]") {
    Input input;
    PadSnapshot first;
    first.connected = true;
    first.buttons[static_cast<usize>(PadButton::B)] = true;
    PadSnapshot second;
    second.connected = true;
    second.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, first);
    input.setPad(1, second);
    input.setKey(Key::Space, true);
    REQUIRE_FALSE(readPlayButtons(input, {}, true, 0).turboAttackPressed);
    REQUIRE_FALSE(readPlayButtons(input, {}, false, kAllPads).turboAttackPressed);
    REQUIRE_FALSE(readPlayButtons(input, {}, false, 1).turbo);
    first.buttons[static_cast<usize>(PadButton::B)] = false;
    first.buttons[static_cast<usize>(PadButton::X)] = true;
    input.setPad(0, first);
    REQUIRE_FALSE(readPlayButtons(input, {}, false, kAllPads).throwPotion);
}

struct GestureFixture {
    Input input;
    PlayBindings bindings;
    PlayerControlReader reader;
    PadSnapshot pad;

    GestureFixture() { pad.connected = true; }
    PlayButtons step(bool magic, f32 seconds = 0.05f) {
        input.beginPoll();
        pad.buttons[static_cast<usize>(bindings.padUsePotion.front())] = magic;
        input.setPad(0, pad);
        return reader.read(input, bindings, false, 0, seconds);
    }
};

TEST_CASE("pad magic distinguishes a tap, hold and double tap without spending twice",
          "[game][players][controls]") {
    GestureFixture f;
    f.bindings.padUsePotion = {PadButton::LeftBumper};
    REQUIRE_FALSE(f.step(true).usePotion);
    SECTION("tap waits for the configured double-tap window") {
        f.bindings.magicDoubleTapSeconds = 0.4f;
        REQUIRE_FALSE(f.step(false).usePotion);
        REQUIRE_FALSE(f.step(false, 0.3f).usePotion);
        REQUIRE(f.step(false, 0.11f).usePotion);
        REQUIRE_FALSE(f.step(false).usePotion);
    }
    SECTION("hold throws once, with no initial radial cast") {
        f.bindings.magicHoldSeconds = 0.4f;
        REQUIRE_FALSE(f.step(true, 0.3f).throwPotion);
        auto buttons = f.step(true, 0.11f);
        REQUIRE(buttons.throwPotion);
        REQUIRE_FALSE(buttons.usePotion);
        REQUIRE(f.step(true, 1.0f).throwPotion); // animator's hold latch prevents repeated spending
        REQUIRE_FALSE(f.step(false).throwPotion);
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
    }
    SECTION("second press requests a shield, not two casts") {
        REQUIRE_FALSE(f.step(false).usePotion);
        auto buttons = f.step(true);
        REQUIRE(buttons.shieldPotion);
        REQUIRE_FALSE(buttons.usePotion);
        REQUIRE(f.step(true, 1.0f).shieldPotion);
        REQUIRE_FALSE(f.step(false).shieldPotion);
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
    }
    SECTION("explicit chord cancels pending tap and cannot generate a release cast") {
        f.pad.buttons[static_cast<usize>(PadButton::A)] = true;
        REQUIRE(f.step(true).throwPotion);
        f.pad.buttons[static_cast<usize>(PadButton::A)] = false;
        REQUIRE_FALSE(f.step(true, 1.0f).usePotion);
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
    }
    SECTION("disconnect or owner reassignment forgets pending magic") {
        f.step(false);
        f.pad.connected = false;
        f.step(false);
        f.pad.connected = true;
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
        f.step(true);
        f.step(false);
        f.reader.read(f.input, f.bindings, false, 1, 0.01f);
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
    }
    SECTION("reset cancels pending magic") {
        f.step(false);
        f.reader.reset();
        REQUIRE_FALSE(f.step(false, 1.0f).usePotion);
    }
    SECTION("gestures can be disabled for immediate custom bindings") {
        f.bindings.padMagicGestures = false;
        REQUIRE(f.step(true).usePotion);
    }
}

} // namespace
