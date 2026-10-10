#pragma once

#include <optional>

#include "game/menu/MenuInput.h"
#include "game/players/PlayerControls.h"
#include "game/screens/PowerupSelector.h"

namespace gdl::game {

/** One player's input for a simulation update. Local UI fields are never serialized. */
struct PlayInput {
    MoveInput move;
    std::optional<Vec3> aimPoint; ///< mouse aiming, absent for retail controller movement
    MenuInput menu;
    bool movieSkipPressed = false; ///< assigned-device online vote; never a gameplay datagram
    bool attack = false;           ///< the attack button is held
    bool usePotion = false;
    bool throwPotion = false;
    bool shieldPotion = false;       ///< the shield potion button is held
    bool strafe = false;             ///< the strafe button is held
    bool strongAttack = false;       ///< the slow attack button is held
    bool turbo = false;              ///< the turbo button is held
    bool defendPressed = false;      ///< one guard gesture per turbo/defend press
    bool combo = false;              ///< the combo button is held: a partner ahead is taken hold of
    bool chargePressed = false;      ///< the charge button went down this frame
    bool attackPressed = false;      ///< the attack button went down this frame
    bool turboAttackPressed = false; ///< resolved same-device chord, not two merged buttons
    SelectorInput selector;          ///< this frame's presses for the powerup selector
};

} // namespace gdl::game
