#include "game/MenuInput.h"

namespace gdl::game {

MenuInput readMenuInput(const Input& input) {
    MenuInput out;
    out.up = input.wasKeyPressed(Key::Up) || input.wasKeyPressed(Key::W);
    out.down = input.wasKeyPressed(Key::Down) || input.wasKeyPressed(Key::S);
    out.select = input.wasKeyPressed(Key::Enter) || input.wasKeyPressed(Key::Space);
    out.back = input.wasKeyPressed(Key::Backspace);
    out.start = input.wasKeyPressed(Key::Enter);
    for (int pad = 0; pad < Input::kMaxPads; ++pad) {
        out.up = out.up || input.wasPadButtonPressed(pad, PadButton::DpadUp);
        out.down = out.down || input.wasPadButtonPressed(pad, PadButton::DpadDown);
        out.select = out.select || input.wasPadButtonPressed(pad, PadButton::A);
        out.back = out.back || input.wasPadButtonPressed(pad, PadButton::B);
        out.start = out.start || input.wasPadButtonPressed(pad, PadButton::Start);
    }
    return out;
}

} // namespace gdl::game
