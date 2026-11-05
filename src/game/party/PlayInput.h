#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/PlayerControls.h"
#include "game/widgets/MenuInput.h"

namespace gdl::game {

/** One player's input for a simulation update. Local UI fields are never serialized. */
struct PlayInput {
    MoveInput move;
    Vec3 consoleFlight{0}; ///< world-space noclip wish; ignored unless the seat's noclip is on
    std::optional<Vec3>
        aimDirection; ///< horizontal world facing; zero retains yaw, absent for a stick
    MenuInput menu;
    bool movieSkipPressed = false; ///< assigned-device online vote; never a gameplay datagram
    bool attack = false;           ///< the attack button is held
    bool usePotion = false;
    bool throwPotion = false;
    bool shieldPotion = false; ///< the shield potion button is held
    bool strafe = false;       ///< the strafe button is held
    bool freeMove = false;     ///< Robotron: the keys walk the camera's way; the mouse aims attacks
    bool strongAttack = false; ///< the slow attack button is held
    bool turbo = false;        ///< the turbo button is held
    bool defendPressed = false;      ///< one guard gesture per turbo/defend press
    bool combo = false;              ///< the combo button is held: a partner ahead is taken hold of
    bool chargePressed = false;      ///< the charge button went down this frame
    bool attackPressed = false;      ///< the attack button went down this frame
    bool turboAttackPressed = false; ///< resolved same-device chord, not two merged buttons
    SelectorInput selector;          ///< this frame's presses for the powerup selector
    u8 hintTopic = 0;                ///< captured Sumner row + 1; never a pointer coordinate
    u8 hintRevision = 0;             ///< menu revision observed when the row was clicked
};

} // namespace gdl::game
