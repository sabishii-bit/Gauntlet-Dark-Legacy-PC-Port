#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/party/PlayerRuntime.h"

namespace gdl::game {

/**
 * The party's members keep out of one another (PlayerCollidePlayers, pmotion.c 4035): a step
 * that runs into another standing member, one whose middle lies within half the mover's height
 * of its own, ends pushed out to their whole widths' sum from them, and the one run into is
 * shoved by the step as it was meant, once the level's items have held it off but before walls
 * or creatures stop or turn it (pmotion.c 1124-1175); it yields to the shove slowly and shows
 * being pushed. After the walls and the floor a second pass meets them at nine tenths of the
 * mover's width and only trims the step toward that, shoving nobody (pmotion.c 1320-1353).
 */
class PartyCollision {
public:
    static constexpr f32 kCoincident = 0.001f; ///< too near to push out from: the step is undone
    static constexpr f32 kBlockShare = 0.9f;   ///< of the mover's width, the blocking pass's

    /** Keeps `mover`'s step from `from` to `to` out of the others by its whole width and
     * theirs, the nearest of those it runs into deciding; returns who that was, `to`
     * resolved. */
    static std::optional<usize> resolve(std::span<const PlayerRuntime> players, usize mover,
                                        const Vec3& from, Vec3& to);
    /** The blocking pass over `mover`'s step from `from` to `to`: the first in seat order it
     * runs into at kBlockShare of its width trims `to` toward the point out of them, per axis
     * as PlayerCollideWalls trims (pmotion.c 3262-3302). Returns the way out the trim leaves
     * for the mover's push velocity (Knockback::pushOut); nobody is shoved. */
    static Vec3 block(std::span<const PlayerRuntime> players, usize mover, const Vec3& from,
                      Vec3& to);
    /** Meets `mover`'s step of `seconds` from `from` to `meant`, as it was meant: whoever it
     * runs into is shoved by it (shove), and the mover goes from where its walls left it to
     * the point out of them, as the original puts the step's end out before its walls run.
     * Returns the contacted party index. */
    static std::optional<usize> step(std::span<PlayerRuntime> players, usize mover,
                                     const Vec3& from, const Vec3& meant, f32 seconds,
                                     const WorldCollision* collision = nullptr);
    /** A member run into by a step that moved `displacement` over `seconds` is shoved by it,
     * unless it stands on an exit waiting for the rest of the party (idle_timer, +0x950,
     * pmotion.c 1166), which guards a walker's step and a pinball's alike. */
    static void shove(PlayerRuntime& other, const Vec3& displacement, f32 seconds);
};

} // namespace gdl::game
