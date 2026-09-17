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

/** Keys a text field claims: they type or erase instead of steering the menu. */
bool typesText(Key key) {
    return (key >= Key::A && key <= Key::Z) || (key >= Key::Num0 && key <= Key::Num9) ||
           key == Key::Space || key == Key::Backspace;
}

/** Whether `key` still steers the menu for `source`. */
bool steers(Key key, const MenuInputSource& source) {
    return source.keyboard && !(source.text && typesText(key));
}

bool anyKeyPressed(const Input& input, std::span<const Key> keys, const MenuInputSource& source) {
    return std::ranges::any_of(
        keys, [&](Key key) { return steers(key, source) && input.wasKeyPressed(key); });
}

bool anyKeyDown(const Input& input, std::span<const Key> keys, const MenuInputSource& source) {
    return std::ranges::any_of(
        keys, [&](Key key) { return steers(key, source) && input.isKeyDown(key); });
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

constexpr u32 kFirstPrintable = 0x20;
constexpr u32 kLastPrintable = 0x7E;

} // namespace

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings, MenuInputSource source) {
    const auto pressed = [&](const std::vector<Key>& keys, const std::vector<PadButton>& buttons) {
        return anyKeyPressed(input, keys, source) ||
               anyButtonPressed(input, buttons, source.pad);
    };
    const auto held = [&](const std::vector<Key>& keys, const std::vector<PadButton>& buttons) {
        return anyKeyDown(input, keys, source) ||
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
    if (source.keyboard && source.text) {
        for (const u32 codepoint : input.typedText()) {
            if (codepoint >= kFirstPrintable && codepoint <= kLastPrintable) {
                out.typed.push_back(static_cast<char>(codepoint));
            }
        }
        out.erase = input.wasKeyPressed(Key::Backspace);
    }
    return out;
}

} // namespace gdl::game
