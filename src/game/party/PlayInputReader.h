#pragma once

#include <array>
#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/config/ControlProfiles.h"
#include "game/party/PlayInput.h"
#include "game/players/CursorAim.h"
#include "game/players/PlayerControls.h"
#include "game/widgets/SceneInput.h"

namespace gdl::game {

/** This machine's play input, one entry per input slot: its stick, its buttons through their
 * tap and hold gestures, the cursor's aim once the mouse takes over, its menu presses and
 * movie vote. Time advances only for the frames it reads. */
class PlayInputReader {
public:
    using Frame = std::array<PlayInput, kInputSlots>; ///< indexed by input slot

    /** Where an input slot's cursor turns the character it plays. */
    class Aim {
    public:
        virtual ~Aim() = default;
        /** The facing that turns the character `inputSlot` plays toward `cursor`: zero keeps
         * the facing it has, none leaves the facing to the stick. */
        virtual std::optional<Vec3> toward(s32 inputSlot, Vec2 cursor) const = 0;
    };

    /** Every input slot's controls from this update's devices; their gestures advance by
     * `seconds`. */
    Frame read(const SceneInput& input, f64 seconds, const Aim& aim);
    /** Forgets the gestures under way; the cursor keeps whose it is. */
    void reset();

private:
    std::array<PlayerControlReader, kInputSlots> m_buttons;
    std::array<CursorInput, kInputSlots> m_cursors;
};

} // namespace gdl::game
