#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** One frame of movement: a unit direction (x right, y forward, as on a stick) and how hard. */
struct MoveInput {
    Vec2 direction{0.0f, 0.0f};
    f32 magnitude = 0.0f;

    bool any() const { return magnitude > 0.0f; }
};

/**
 * Reads a player's movement from the keyboard (when `keyboard`) and the pad `pad`
 * (`kAllPads` for every pad, `kNoPad` for none): the stick past its dead zone, the
 * directional buttons, and the bound keys, added together and clamped to full deflection.
 */
MoveInput readMoveInput(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad);

/** A player's buttons for a frame of play: what is held, and the selector's presses. */
struct PlayButtons {
    bool attack = false;
    bool usePotion = false;
    bool throwPotion = false;
    bool shieldPotion = false;  ///< held
    bool strafe = false;        ///< held
    bool strongAttack = false;  ///< held
    bool turbo = false;         ///< held
    bool chargePressed = false; ///< true only the frame it goes down
    bool attackPressed = false; ///< likewise
    bool selectorUp = false;    ///< these four true only the frame they go down
    bool selectorDown = false;
    bool selectorLeft = false;
    bool selectorRight = false;
};

PlayButtons readPlayButtons(const Input& input, const PlayBindings& bindings, bool keyboard,
                            s32 pad);

/** Whether the player holds their attack: a bound key (when `keyboard`) or pad button. */
bool readAttackInput(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad);

inline constexpr s32 kAllPads = -1;
inline constexpr s32 kNoPad = -2;

} // namespace gdl::game
