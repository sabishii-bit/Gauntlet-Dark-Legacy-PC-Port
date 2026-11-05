#include "game/party/PlayInputReader.h"

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/party/PlayInput.h"
#include "game/players/PlayerControls.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/SceneInput.h"

namespace gdl::game {

PlayInputReader::Frame PlayInputReader::read(const SceneInput& input, f64 seconds, const Aim& aim) {
    Frame frame{};
    for (s32 slot = 0; slot < static_cast<s32>(kInputSlots); ++slot) {
        const auto index = static_cast<usize>(slot);
        const MenuInputSource source = input.source(slot);
        const auto& bindings = playBindings(input.config(), slot);
        PlayInput& in = frame[index];
        in.move = readMoveInput(input.devices(), bindings, source.keyboard, source.pad);
        in.freeMove =
            source.keyboard && input.config().controls[index].scheme == KeyboardScheme::Robotron;
        if (m_cursors[index].update(input.devices(), bindings, source.keyboard, source.pad)) {
            const Vec2 cursor{input.devices().pointer().x, input.devices().pointer().y};
            in.aimDirection = aim.toward(slot, cursor);
        }
        const PlayButtons buttons = m_buttons[index].read(
            input.devices(), bindings, source.keyboard, source.pad, static_cast<f32>(seconds));
        in.attack = buttons.attack;
        in.usePotion = buttons.usePotion;
        in.throwPotion = buttons.throwPotion;
        in.shieldPotion = buttons.shieldPotion;
        in.strafe = buttons.strafe;
        in.strongAttack = buttons.strongAttack;
        in.turbo = buttons.turbo;
        in.defendPressed = buttons.defendPressed;
        in.combo = buttons.combo;
        in.chargePressed = buttons.chargePressed;
        in.attackPressed = buttons.attackPressed;
        in.turboAttackPressed = buttons.turboAttackPressed;
        in.selector = SelectorInput{buttons.selectorUp, buttons.selectorDown, buttons.selectorLeft,
                                    buttons.selectorRight};
        in.menu = input.slot(slot);
        in.movieSkipPressed = input.movieVote(slot);
    }
    return frame;
}

void PlayInputReader::reset() {
    for (PlayerControlReader& buttons : m_buttons) {
        buttons.reset();
    }
}

} // namespace gdl::game
