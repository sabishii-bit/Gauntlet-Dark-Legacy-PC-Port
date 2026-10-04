#pragma once

#include <optional>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** One frame of menu commands from the keyboard and/or pads: presses, which directions are
 * still held for auto-repeat, and what a text field with the keyboard received. */
struct MenuInput {
    std::optional<Vec2> pointer; ///< canvas coordinates after mapMenuPointer
    f32 pointerScroll = 0;       ///< wheel motion, positive upward
    bool pointerNormalized = false;
    bool pointerPressed = false;
    bool pointerHeld = false;
    bool pointerBack = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool select = false; ///< confirm the highlighted item
    bool back = false;
    bool start = false; ///< the Start button or its keyboard binding
    bool upHeld = false;
    bool downHeld = false;
    bool leftHeld = false;
    bool rightHeld = false;
    std::string typed;   ///< printable characters typed into a text field this frame
    bool erase = false;  ///< Backspace, for a text field
    bool escape = false; ///< the escape binding: leaves a text field, quits elsewhere

    bool any() const {
        return up || down || left || right || select || back || start || pointerPressed ||
               pointerBack || pointerScroll != 0;
    }
};

/** Which devices one read merges: the keyboard and one pad, or every pad. */
struct MenuInputSource {
    static constexpr s32 kAllPads = -1;
    static constexpr s32 kNoPad = -2;
    static constexpr s32 kKeyboardPlayer = 0;

    bool keyboard = true;
    s32 pad = kAllPads;
    bool text = false; ///< a text field has the keyboard: letter, digit, space and Backspace
                       ///< keys type or erase instead of steering

    /** The devices that speak for a player: the keyboard belongs to the first. */
    static MenuInputSource forPlayer(s32 player) {
        return MenuInputSource{player == kKeyboardPlayer, player};
    }

    /** The same devices with the keyboard typing into a text field. */
    MenuInputSource typing() const {
        MenuInputSource source = *this;
        source.text = true;
        return source;
    }
};

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings,
                        MenuInputSource source = {});

/** Maps a window-normalized cursor through the same virtual transform used for drawing. */
MenuInput mapMenuPointer(MenuInput input, const Mat4& canvasTransform);

/** Route mouse input only to a canvas region, leaving keyboard/controller commands intact. */
MenuInput mapMenuPointer(MenuInput input, const Mat4& canvasTransform, const Rect& region);

} // namespace gdl::game
