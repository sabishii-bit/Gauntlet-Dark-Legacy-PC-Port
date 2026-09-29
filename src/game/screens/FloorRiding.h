#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/**
 * Members ride the moving floors they stand on (PlayerCheckFloor, pmotion.c 4271): the
 * original parents the body to a floor object flagged to move, so a lift carries it up and a
 * turntable turns it round with its facing. A member may not step onto a moving floor while
 * another rides a different one kept apart (flag 0x4000, OtherPlayerOnOtherMovingObject): the
 * step is refused.
 */
class FloorRiding {
public:
    static constexpr u32 kMoving = 0x1000;    ///< a floor object that moves
    static constexpr u32 kKeptApart = 0x4000; ///< one member at a time on it
    static constexpr f32 kProbeAbove = 0.5f;  ///< the floor under a body is looked for from
    static constexpr f32 kProbeBelow = 1.0f;  ///< this over its feet to this under them

    /** Moves the member by how its moving floor moved since the floor was noted. */
    static void carry(PlayerRuntime& runtime, const WorldCollision& collision);
    /** After `index` stepped from `from`: refuses the step when another rides a floor kept
     * apart, then notes the floor it stands on. */
    static void land(std::span<PlayerRuntime> players, usize index, const Vec3& from,
                     const WorldCollision& collision);
    /** Whether another standing member stands on a floor kept apart other than `object`. */
    static bool keptApart(std::span<const PlayerRuntime> players, usize index, s32 object);
};

} // namespace gdl::game
