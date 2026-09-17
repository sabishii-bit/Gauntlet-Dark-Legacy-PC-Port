#pragma once

#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** One frame of menu commands from the keyboard and/or pads: presses, plus which directions
 * are still held for auto-repeat. */
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

    bool any() const { return up || down || left || right || select || back || start; }
};

/** Which devices one read merges: the keyboard and one pad, or every pad. */
struct MenuInputSource {
    static constexpr int kAllPads = -1;
    static constexpr int kNoPad = -2;

    bool keyboard = true;
    int pad = kAllPads;

    /** The devices that speak for a player: the keyboard belongs to the first. */
    static MenuInputSource forPlayer(int player) { return MenuInputSource{player == 0, player}; }
};

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings,
                        MenuInputSource source = {});

} // namespace gdl::game
