#pragma once

#include <string>

#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** One frame of menu commands from the keyboard and/or pads: presses, which directions are
 * still held for auto-repeat, and what a text field with the keyboard received. */
struct MenuInput {
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
    std::string typed; ///< printable characters typed into a text field this frame
    bool erase = false; ///< Backspace, for a text field
    bool escape = false; ///< the escape binding: leaves a text field, quits elsewhere

    bool any() const { return up || down || left || right || select || back || start; }
};

/** Which devices one read merges: the keyboard and one pad, or every pad. */
struct MenuInputSource {
    static constexpr int kAllPads = -1;
    static constexpr int kNoPad = -2;
    static constexpr int kKeyboardPlayer = 0;

    bool keyboard = true;
    int pad = kAllPads;
    bool text = false; ///< a text field has the keyboard: letter, digit, space and Backspace
                       ///< keys type or erase instead of steering

    /** The devices that speak for a player: the keyboard belongs to the first. */
    static MenuInputSource forPlayer(int player) {
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

} // namespace gdl::game
