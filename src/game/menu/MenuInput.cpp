#include "game/menu/MenuInput.h"

#include <algorithm>
#include <span>

namespace gdl::game {

namespace {

bool anyKeyPressed(const Input& input, std::span<const Key> keys) {
    return std::ranges::any_of(keys, [&](Key key) { return input.wasKeyPressed(key); });
}

bool anyButtonPressed(const Input& input, std::span<const PadButton> buttons) {
    for (int pad = 0; pad < Input::kMaxPads; ++pad) {
        if (std::ranges::any_of(buttons, [&](PadButton button) {
                return input.wasPadButtonPressed(pad, button);
            })) {
            return true;
        }
    }
    return false;
}

} // namespace

MenuInput readMenuInput(const Input& input, const MenuBindings& bindings) {
    MenuInput out;
    out.up = anyKeyPressed(input, bindings.up) || anyButtonPressed(input, bindings.padUp);
    out.down = anyKeyPressed(input, bindings.down) || anyButtonPressed(input, bindings.padDown);
    out.select =
        anyKeyPressed(input, bindings.select) || anyButtonPressed(input, bindings.padSelect);
    out.back = anyKeyPressed(input, bindings.back) || anyButtonPressed(input, bindings.padBack);
    out.start = anyKeyPressed(input, bindings.start) || anyButtonPressed(input, bindings.padStart);
    return out;
}

} // namespace gdl::game
