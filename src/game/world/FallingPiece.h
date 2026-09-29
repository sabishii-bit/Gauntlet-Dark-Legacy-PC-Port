#pragma once

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** How a loose piece of a level falls: what its rise loses each 30 Hz frame and how fast
 * it tumbles. The level items that give way share three of these by subtype. */
struct FallingProfile {
    f32 gravityStep = 2.0f; ///< taken off the vertical velocity each frame
    f32 spin = 0.34906585f; ///< radians a second of pitch and roll, twenty degrees

    static constexpr s32 kLeafFall = 49;
    static constexpr s32 kRockSink = 53;
    /** The profile of an item subtype: leaves fall at half the pull and ten degrees a
     * second, sinking rocks turn only a degree a second, everything else twenty. */
    static FallingProfile of(s32 subtype);
};

/** One piece of a level once it falls: it drops under its profile's pull, pitches and
 * rolls at its profile's rate in the directions its instance index picks from the
 * original's table, and is done with once it is two hundred under the world's floor
 * sentinel. */
class FallingPiece {
public:
    static constexpr f32 kStep = 1.0f / 30.0f;
    static constexpr f32 kDiscardDepth = 200.0f;
    static constexpr f32 kFloorSentinelMargin = 4.5f; ///< the floor sentinel under worldmin

    /** Where the pieces of `layout` are done with: 200 under its floor sentinel. */
    static f32 bottomOf(const WorldLayout& layout);
    /** How many whole 30 Hz frames `seconds` brings due, carrying the rest in `remainder`. */
    static s32 framesDue(f32& remainder, f32 seconds);

    /** Stands the piece at rest at `where`, turned by `angles`, as instance `index`. */
    void place(const Vec3& where, const Vec3& angles, usize index);
    /** One 30 Hz frame of falling; the piece is no longer visible under `bottom`. */
    void advance(const FallingProfile& profile, f32 bottom);

    Vec3 position{0.0f};
    Vec3 rotation{0.0f}; ///< pitch, yaw, roll
    Vec3 velocity{0.0f};
    usize instance = 0;
    bool visible = true;
};
} // namespace gdl::game
