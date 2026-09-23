#include "game/players/PlayerControls.h"

#include <algorithm>
#include <span>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

bool anyKeyDown(const Input& input, std::span<const Key> keys) {
    return std::ranges::any_of(keys, [&](Key key) { return input.isKeyDown(key); });
}

bool anyButtonDown(const Input& input, s32 pad, std::span<const PadButton> buttons) {
    return std::ranges::any_of(
        buttons, [&](PadButton button) { return input.isPadButtonDown(pad, button); });
}

/** The stick's deflection beyond the dead zone, rescaled so full tilt stays 1. */
Vec2 stick(const Input& input, s32 pad, f32 deadZone) {
    // The pad's y axis grows downwards; forward is up.
    const Vec2 raw{input.padAxis(pad, PadAxis::LeftX), -input.padAxis(pad, PadAxis::LeftY)};
    const f32 length = glm::length(raw);
    if (length <= deadZone || length <= 0.0f) {
        return Vec2{0.0f, 0.0f};
    }
    const f32 usable = std::clamp(deadZone, 0.0f, 0.99f);
    const f32 scaled = std::min(1.0f, (length - usable) / (1.0f - usable));
    return raw / length * scaled;
}

} // namespace

MoveInput readMoveInput(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad) {
    Vec2 sum{0.0f, 0.0f};
    if (keyboard) {
        sum.x += anyKeyDown(input, bindings.right) ? 1.0f : 0.0f;
        sum.x -= anyKeyDown(input, bindings.left) ? 1.0f : 0.0f;
        sum.y += anyKeyDown(input, bindings.up) ? 1.0f : 0.0f;
        sum.y -= anyKeyDown(input, bindings.down) ? 1.0f : 0.0f;
    }
    s32 first = pad;
    s32 last = pad;
    if (pad == kAllPads) {
        first = 0;
        last = Input::kMaxPads - 1;
    } else if (pad == kNoPad) {
        last = first - 1;
    }
    for (s32 index = first; index <= last; ++index) {
        if (!input.isPadConnected(index)) {
            continue;
        }
        sum += stick(input, index, bindings.stickDeadZone);
        sum.x += anyButtonDown(input, index, bindings.padRight) ? 1.0f : 0.0f;
        sum.x -= anyButtonDown(input, index, bindings.padLeft) ? 1.0f : 0.0f;
        sum.y += anyButtonDown(input, index, bindings.padUp) ? 1.0f : 0.0f;
        sum.y -= anyButtonDown(input, index, bindings.padDown) ? 1.0f : 0.0f;
    }
    MoveInput out;
    const f32 length = glm::length(sum);
    if (length > 0.0f) {
        out.direction = sum / length;
        out.magnitude = std::min(1.0f, length);
    }
    return out;
}

namespace {

/** Whether any of the keys (with the keyboard) or of the buttons on the player's pads is
 * down, or with `edge` went down this frame. */
bool bound(const Input& input, std::span<const Key> keys, std::span<const PadButton> buttons,
           bool keyboard, s32 pad, bool edge) {
    if (keyboard && std::ranges::any_of(keys, [&](Key key) {
            return edge ? input.wasKeyPressed(key) : input.isKeyDown(key);
        })) {
        return true;
    }
    s32 first = pad;
    s32 last = pad;
    if (pad == kAllPads) {
        first = 0;
        last = Input::kMaxPads - 1;
    } else if (pad == kNoPad) {
        last = first - 1;
    }
    for (s32 index = first; index <= last; ++index) {
        if (input.isPadConnected(index) && std::ranges::any_of(buttons, [&](PadButton button) {
                return edge ? input.wasPadButtonPressed(index, button)
                            : input.isPadButtonDown(index, button);
            })) {
            return true;
        }
    }
    return false;
}

} // namespace

bool readAttackInput(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad) {
    return bound(input, bindings.attack, bindings.padAttack, keyboard, pad, false);
}

namespace {

// Resolve chords BEFORE merging devices, so one player's buttons cannot complete another's.
PlayButtons deviceButtons(const Input& input, const PlayBindings& b, bool keyboard, s32 pad) {
    PlayButtons out;
    out.attack = bound(input, b.attack, b.padAttack, keyboard, pad, false);
    out.usePotion = bound(input, b.usePotion, b.padUsePotion, keyboard, pad, false);
    out.throwPotion = bound(input, b.throwPotion, b.padThrowPotion, keyboard, pad, false);
    out.shieldPotion = bound(input, b.shieldPotion, b.padShieldPotion, keyboard, pad, false);
    out.strafe = bound(input, b.strafe, b.padStrafe, keyboard, pad, false);
    out.strongAttack = bound(input, b.strongAttack, b.padStrongAttack, keyboard, pad, false);
    out.turbo = bound(input, b.turbo, b.padTurbo, keyboard, pad, false);
    out.chargePressed = bound(input, b.charge, b.padCharge, keyboard, pad, true);
    out.attackPressed = bound(input, b.attack, b.padAttack, keyboard, pad, true);
    out.selectorUp = bound(input, b.selectorUp, b.padSelectorUp, keyboard, pad, true);
    out.selectorDown = bound(input, b.selectorDown, b.padSelectorDown, keyboard, pad, true);
    out.selectorLeft = bound(input, b.selectorLeft, b.padSelectorLeft, keyboard, pad, true);
    out.selectorRight = bound(input, b.selectorRight, b.padSelectorRight, keyboard, pad, true);
    if (b.actionChords && out.usePotion && (out.attack || out.turbo)) {
        out.shieldPotion = out.shieldPotion || out.turbo;
        out.throwPotion = out.throwPotion || (!out.turbo && out.attack);
        out.usePotion = false;
        out.attack = false;
        out.attackPressed = false;
        out.strongAttack = false;
        out.turbo = false;
    }
    out.turboAttackPressed = b.actionChords && out.turbo && out.attackPressed;
    return out;
}

void mergeButtons(PlayButtons& to, const PlayButtons& from) {
    constexpr auto kFields = std::to_array<bool PlayButtons::*>(
        {&PlayButtons::attack, &PlayButtons::usePotion, &PlayButtons::throwPotion,
         &PlayButtons::shieldPotion, &PlayButtons::strafe, &PlayButtons::strongAttack,
         &PlayButtons::turbo, &PlayButtons::chargePressed, &PlayButtons::attackPressed,
         &PlayButtons::turboAttackPressed, &PlayButtons::selectorUp, &PlayButtons::selectorDown,
         &PlayButtons::selectorLeft, &PlayButtons::selectorRight});
    for (const auto field : kFields) {
        to.*field = to.*field || from.*field;
    }
}

} // namespace

PlayButtons readPlayButtons(const Input& input, const PlayBindings& b, bool keyboard, s32 pad) {
    PlayButtons out = deviceButtons(input, b, keyboard, kNoPad);
    for (s32 i = 0; i < Input::kMaxPads; ++i) {
        if ((pad == i || pad == kAllPads) && input.isPadConnected(i)) {
            mergeButtons(out, deviceButtons(input, b, false, i));
        }
    }
    return out;
}

void PlayerControlReader::MagicGesture::apply(PlayButtons& buttons, bool down, bool pressed,
                                              f32 elapsed, const PlayBindings& bindings) {
    if (buttons.throwPotion || buttons.shieldPotion) {
        phase = Phase::Consumed;
        buttons.usePotion = false;
        return;
    }
    buttons.usePotion = false;
    seconds += std::max(0.0f, elapsed);
    switch (phase) {
    case Phase::Idle:
        if (pressed) {
            phase = Phase::Pressed;
            seconds = 0.0f;
        }
        break;
    case Phase::Pressed:
        if (!down) {
            phase = Phase::Released;
            seconds = 0.0f;
        } else if (seconds >= bindings.magicHoldSeconds) {
            phase = Phase::Throw;
            buttons.throwPotion = true;
        }
        break;
    case Phase::Released:
        if (pressed && seconds <= bindings.magicDoubleTapSeconds) {
            phase = Phase::Shield;
            buttons.shieldPotion = true;
        } else if (seconds >= bindings.magicDoubleTapSeconds) {
            buttons.usePotion = true;
            phase = pressed ? Phase::Pressed : Phase::Idle;
            seconds = 0.0f;
        }
        break;
    case Phase::Throw:
        buttons.throwPotion = down;
        if (!down) {
            phase = Phase::Idle;
        }
        break;
    case Phase::Shield:
        buttons.shieldPotion = down;
        if (!down) {
            phase = Phase::Idle;
        }
        break;
    case Phase::Consumed:
        if (!down) {
            phase = Phase::Idle;
        }
        break;
    }
}

PlayButtons PlayerControlReader::read(const Input& input, const PlayBindings& b, bool keyboard,
                                      s32 pad, f32 seconds) {
    PlayButtons out = deviceButtons(input, b, keyboard, kNoPad);
    for (s32 i = 0; i < Input::kMaxPads; ++i) {
        if ((pad != i && pad != kAllPads) || !input.isPadConnected(i)) {
            m_magic[static_cast<usize>(i)] = {};
            continue;
        }
        PlayButtons buttons = deviceButtons(input, b, false, i);
        const bool down = bound(input, {}, b.padUsePotion, false, i, false);
        const bool pressed = bound(input, {}, b.padUsePotion, false, i, true);
        if (b.padMagicGestures) {
            m_magic[static_cast<usize>(i)].apply(buttons, down, pressed, seconds, b);
        } else {
            m_magic[static_cast<usize>(i)] = {};
        }
        mergeButtons(out, buttons);
    }
    return out;
}

void PlayerControlReader::reset() {
    m_magic = {};
}

} // namespace gdl::game
