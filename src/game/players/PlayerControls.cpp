#include "game/players/PlayerControls.h"

#include <algorithm>
#include <span>

namespace gdl::game {

namespace {

bool anyKeyDown(const Input& input, std::span<const Key> keys) {
    return std::ranges::any_of(keys, [&](Key key) { return input.isKeyDown(key); });
}

bool anyButtonDown(const Input& input, int pad, std::span<const PadButton> buttons) {
    return std::ranges::any_of(
        buttons, [&](PadButton button) { return input.isPadButtonDown(pad, button); });
}

/** The stick's deflection beyond the dead zone, rescaled so full tilt stays 1. */
Vec2 stick(const Input& input, int pad, f32 deadZone) {
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

MoveInput readMoveInput(const Input& input, const PlayBindings& bindings, bool keyboard,
                        int pad) {
    Vec2 sum{0.0f, 0.0f};
    if (keyboard) {
        sum.x += anyKeyDown(input, bindings.right) ? 1.0f : 0.0f;
        sum.x -= anyKeyDown(input, bindings.left) ? 1.0f : 0.0f;
        sum.y += anyKeyDown(input, bindings.up) ? 1.0f : 0.0f;
        sum.y -= anyKeyDown(input, bindings.down) ? 1.0f : 0.0f;
    }
    int first = pad;
    int last = pad;
    if (pad == kAllPads) {
        first = 0;
        last = Input::kMaxPads - 1;
    } else if (pad == kNoPad) {
        last = first - 1;
    }
    for (int index = first; index <= last; ++index) {
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
           bool keyboard, int pad, bool edge) {
    if (keyboard && std::ranges::any_of(keys, [&](Key key) {
            return edge ? input.wasKeyPressed(key) : input.isKeyDown(key);
        })) {
        return true;
    }
    int first = pad;
    int last = pad;
    if (pad == kAllPads) {
        first = 0;
        last = Input::kMaxPads - 1;
    } else if (pad == kNoPad) {
        last = first - 1;
    }
    for (int index = first; index <= last; ++index) {
        if (input.isPadConnected(index) &&
            std::ranges::any_of(buttons, [&](PadButton button) {
                return edge ? input.wasPadButtonPressed(index, button)
                            : input.isPadButtonDown(index, button);
            })) {
            return true;
        }
    }
    return false;
}

} // namespace

bool readAttackInput(const Input& input, const PlayBindings& bindings, bool keyboard, int pad) {
    return bound(input, bindings.attack, bindings.padAttack, keyboard, pad, false);
}

PlayButtons readPlayButtons(const Input& input, const PlayBindings& b, bool keyboard, int pad) {
    PlayButtons out;
    out.attack = bound(input, b.attack, b.padAttack, keyboard, pad, false);
    out.usePotion = bound(input, b.usePotion, b.padUsePotion, keyboard, pad, false);
    out.throwPotion = bound(input, b.throwPotion, b.padThrowPotion, keyboard, pad, false);
    out.turbo = bound(input, b.turbo, b.padTurbo, keyboard, pad, false);
    out.chargePressed = bound(input, b.charge, b.padCharge, keyboard, pad, true);
    out.attackPressed = bound(input, b.attack, b.padAttack, keyboard, pad, true);
    out.selectorUp = bound(input, b.selectorUp, b.padSelectorUp, keyboard, pad, true);
    out.selectorDown = bound(input, b.selectorDown, b.padSelectorDown, keyboard, pad, true);
    out.selectorLeft = bound(input, b.selectorLeft, b.padSelectorLeft, keyboard, pad, true);
    out.selectorRight = bound(input, b.selectorRight, b.padSelectorRight, keyboard, pad, true);
    return out;
}

} // namespace gdl::game
