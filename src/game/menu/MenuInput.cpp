#include "game/menu/MenuInput.h"

#include <algorithm>
#include <span>
#include <utility>

#include "engine/core/Types.h"

#include "game/config/ControlProfiles.h"

namespace gdl::game {

namespace {

/** Pads a read covers: one, or all of them. */
struct PadRange {
    s32 first = 0;
    s32 last = -1;
};

PadRange padsOf(s32 pad) {
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

bool anyButtonPressed(const Input& input, std::span<const PadButton> buttons, s32 pad) {
    const PadRange range = padsOf(pad);
    for (s32 index = range.first; index <= range.last; ++index) {
        if (std::ranges::any_of(buttons, [&](PadButton button) {
                return input.wasPadButtonPressed(index, button);
            })) {
            return true;
        }
    }
    return false;
}

bool anyButtonDown(const Input& input, std::span<const PadButton> buttons, s32 pad) {
    const PadRange range = padsOf(pad);
    for (s32 index = range.first; index <= range.last; ++index) {
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
        return anyKeyPressed(input, keys, source) || anyButtonPressed(input, buttons, source.pad);
    };
    const auto held = [&](const std::vector<Key>& keys, const std::vector<PadButton>& buttons) {
        return anyKeyDown(input, keys, source) || anyButtonDown(input, buttons, source.pad);
    };
    MenuInput out;
    out.devices = &input;
    if (input.pointer().inside) {
        out.pointer = Vec2{input.pointer().x, input.pointer().y};
        out.pointerNormalized = true;
        out.pointerPressed = input.wasPointerPressed();
        out.pointerHeld = input.pointer().down;
        out.pointerBack = input.wasPointerBackPressed();
        out.pointerScroll = input.pointerScroll();
    }
    out.up = pressed(bindings.up, bindings.padUp);
    out.down = pressed(bindings.down, bindings.padDown);
    out.left = pressed(bindings.left, bindings.padLeft);
    out.right = pressed(bindings.right, bindings.padRight);
    out.select = pressed(bindings.select, bindings.padSelect);
    out.back = pressed(bindings.back, bindings.padBack);
    out.start = pressed(bindings.start, bindings.padStart);
    out.escape = pressed(bindings.escape, {});
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

MenuInputSource playerInputSource(const Input& input, const GameConfig& config, s32 player) {
    const auto device = controlDevices(config, input).at(static_cast<usize>(player));
    return {device.keyboard, device.pad};
}
MenuInput readPlayerMenuInput(const Input& input, const GameConfig& config, s32 player,
                              bool typing) {
    auto source = playerInputSource(input, config, player);
    source.text = typing;
    return readMenuInput(input, menuBindings(config, player), source);
}
MenuInput readSharedMenuInput(const Input& input, const GameConfig& config) {
    // Keyboard recovery remains available at the title even if every player is unassigned.
    const auto devices = controlDevices(config, input);
    const bool assignedKeyboard = std::ranges::any_of(devices, &ControlDevice::keyboard);
    auto result = readMenuInput(input, config.menu, {!assignedKeyboard, MenuInputSource::kNoPad});
    for (s32 player = 0; player < 4; ++player) {
        const auto lane = readPlayerMenuInput(input, config, player);
        result.up |= lane.up;
        result.down |= lane.down;
        result.left |= lane.left;
        result.right |= lane.right;
        result.select |= lane.select;
        result.back |= lane.back;
        result.start |= lane.start;
        result.escape |= lane.escape;
        result.upHeld |= lane.upHeld;
        result.downHeld |= lane.downHeld;
        result.leftHeld |= lane.leftHeld;
        result.rightHeld |= lane.rightHeld;
    }
    return result;
}

MenuInput readMovieMenuInput(const Input& input, const GameConfig& config,
                             std::optional<s32> player) {
    auto result =
        player ? readPlayerMenuInput(input, config, *player) : readSharedMenuInput(input, config);
    result.start |= result.pointerPressed;
    return result;
}

MenuInput readPauseMenuInput(const Input& input, const GameConfig& config, s32 owner) {
    const auto source = playerInputSource(input, config, owner);
    return source.keyboard || input.isPadConnected(source.pad)
               ? readPlayerMenuInput(input, config, owner)
               : readSharedMenuInput(input, config);
}

MenuInput mapMenuPointer(MenuInput input, const Mat4& canvasTransform) {
    if (input.pointer && input.pointerNormalized) {
        const auto clip = Vec4{input.pointer->x * 2 - 1, input.pointer->y * 2 - 1, 0.5f, 1};
        const auto point = glm::inverse(canvasTransform) * clip;
        input.pointer = Vec2{point} / point.w;
        input.pointerNormalized = false;
    }
    input.back |= input.pointerBack;
    return input;
}

MenuInput mapMenuPointer(MenuInput input, const Mat4& canvasTransform, const Rect& region) {
    const bool deviceBack = input.back;
    input = mapMenuPointer(std::move(input), canvasTransform);
    if (!input.pointer || input.pointer->x < region.x ||
        input.pointer->x >= region.x + region.width || input.pointer->y < region.y ||
        input.pointer->y >= region.y + region.height) {
        input.pointer.reset();
        input.pointerPressed = false;
        input.pointerHeld = false;
        input.pointerBack = false;
        input.pointerScroll = 0;
        input.back = deviceBack;
    }
    return input;
}

} // namespace gdl::game
