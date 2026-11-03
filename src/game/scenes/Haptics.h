#pragma once

#include "engine/core/Types.h"

#include "game/config/ControlProfiles.h"

namespace gdl::game {
/** The controllers' rumble. `player` is the scene's input slot; an online game maps it to
 * one of this machine's devices first. */
class Haptics {
public:
    virtual ~Haptics() = default;
    /** Shakes `player`'s controller as `cue` asks: damage for its `frames` native countdown,
     * the other cues for their own lengths. */
    virtual void vibrate(s32 player, s32 frames, ControlFeedback cue) = 0;
    /** Stops every controller's motors. */
    virtual void stopRumble() = 0;
};
} // namespace gdl::game
