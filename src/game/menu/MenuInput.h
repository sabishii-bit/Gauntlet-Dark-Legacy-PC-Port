#pragma once

#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** One frame of menu commands, merged from the keyboard and every connected pad. */
struct MenuInput {
    bool up = false;
    bool down = false;
    bool select = false; ///< confirm the highlighted item
    bool back = false;
    bool start = false; ///< the Start button or its keyboard binding

    bool any() const { return up || down || select || back || start; }
};

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings);

} // namespace gdl::game
