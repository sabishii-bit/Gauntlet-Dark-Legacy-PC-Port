#include "game/menu/MenuInput.h"

#include <algorithm>
#include <span>

namespace gdl::game {

namespace {

/** Pads a read covers: one, or all of them. */
struct PadRange {
    int first = 0;
    int last = -1;
};

PadRange padsOf(int pad) {
    if (pad == MenuInputSource::kNoPad) {
        return {};
    }
    if (pad == MenuInputSource::kAllPads) {
        return PadRange{0, Input::kMaxPads - 1};
    }
    return PadRange{pad, pad};
}

bool anyKeyPressed(const Input& input, std::span<const Key> keys, bool keyboard) {
    return keyboard && std::ranges::any_of(keys, [&](Key key) { return input.wasKeyPressed(key); });
}

bool anyKeyDown(const Input& input, std::span<const Key> keys, bool keyboard) {
    return keyboard && std::ranges::any_of(keys, [&](Key key) { return input.isKeyDown(key); });
}

bool anyButtonPressed(const Input& input, std::span<const PadButton> buttons, int pad) {
    const PadRange range = padsOf(pad);
    for (int index = range.first; index <= range.last; ++index) {
        if (std::ranges::any_of(buttons, [&](PadButton button) {
                return input.wasPadButtonPressed(index, button);
            })) {
            return true;
        }
    }
    return false;
}

bool anyButtonDown(const Input& input, std::span<const PadButton> buttons, int pad) {
    const PadRange range = padsOf(pad);
    for (int index = range.first; index <= range.last; ++index) {
        if (std::ranges::any_of(
                buttons, [&](PadButton button) { return input.isPadButtonDown(index, button); })) {
            return true;
        }
    }
    return false;
}

} // namespace

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings, MenuInputSource source) {
    const auto pressed = [&](const std::vector<Key>& keys, const std::vector<PadButton>& buttons) {
        return anyKeyPressed(input, keys, source.keyboard) ||
               anyButtonPressed(input, buttons, source.pad);
    };
    const auto held = [&](const std::vector<Key>& keys, const std::vector<PadButton>& buttons) {
        return anyKeyDown(input, keys, source.keyboard) ||
               anyButtonDown(input, buttons, source.pad);
    };
    MenuInput out;
    out.up = pressed(bindings.up, bindings.padUp);
    out.down = pressed(bindings.down, bindings.padDown);
    out.left = pressed(bindings.left, bindings.padLeft);
    out.right = pressed(bindings.right, bindings.padRight);
    out.select = pressed(bindings.select, bindings.padSelect);
    out.back = pressed(bindings.back, bindings.padBack);
    out.start = pressed(bindings.start, bindings.padStart);
    out.upHeld = held(bindings.up, bindings.padUp);
    out.downHeld = held(bindings.down, bindings.padDown);
    out.leftHeld = held(bindings.left, bindings.padLeft);
    out.rightHeld = held(bindings.right, bindings.padRight);
    return out;
}

} // namespace gdl::game
