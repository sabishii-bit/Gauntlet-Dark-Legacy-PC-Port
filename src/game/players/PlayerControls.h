#pragma once

#include <array>

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
    bool shieldPotion = false;       ///< held
    bool strafe = false;             ///< held
    bool strongAttack = false;       ///< held
    bool turbo = false;              ///< held
    bool chargePressed = false;      ///< true only the frame it goes down
    bool attackPressed = false;      ///< likewise
    bool turboAttackPressed = false; ///< same-device turbo + quick attack, on quick-attack press
    bool selectorUp = false;         ///< these four true only the frame they go down
    bool selectorDown = false;
    bool selectorLeft = false;
    bool selectorRight = false;
};

/** Immediate bindings and action chords. Gameplay uses PlayerControlReader below for gestures. */
PlayButtons readPlayButtons(const Input& input, const PlayBindings& bindings, bool keyboard,
                            s32 pad);

/** Stateful console magic gestures. One reader per player; no shared input history.
 * Pad magic is tap/use, hold/throw, double-tap/shield. Keyboard shortcuts remain immediate.
 * Call once per input poll with elapsed seconds. Reset when changing gameplay input context. */
class PlayerControlReader {
public:
    PlayButtons read(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad,
                     f32 seconds);
    void reset();

private:
    struct MagicGesture {
        enum class Phase : u8 { Idle, Pressed, Released, Throw, Shield, Consumed };
        Phase phase = Phase::Idle;
        f32 seconds = 0.0f;
        void apply(PlayButtons& buttons, bool down, bool pressed, f32 elapsed,
                   const PlayBindings& bindings);
    };
    std::array<MagicGesture, Input::kMaxPads> m_magic{};
};

/** Whether the player holds their attack: a bound key (when `keyboard`) or pad button. */
bool readAttackInput(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad);

inline constexpr s32 kAllPads = -1;
inline constexpr s32 kNoPad = -2;

} // namespace gdl::game
