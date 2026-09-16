#pragma once

#include "engine/platform/Input.h"

namespace gdl::game {

/** One frame of menu commands, merged from the keyboard and every connected pad. */
struct MenuInput {
    bool up = false;
    bool down = false;
    bool select = false; ///< confirm the highlighted item
    bool back = false;
    bool start = false; ///< the Start button, or Enter

    bool any() const { return up || down || select || back || start; }
};

MenuInput readMenuInput(const Input& input);

} // namespace gdl::game
