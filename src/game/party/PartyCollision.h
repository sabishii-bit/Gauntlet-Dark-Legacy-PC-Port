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

    /** Keeps `mover`'s step from `from` to `to` out of the others by `share` of its width and
     * the whole of theirs, the nearest of those it runs into deciding; returns who that was,
     * `to` resolved. */
    static std::optional<usize> resolve(std::span<const PlayerRuntime> players, usize mover,
                                        const Vec3& from, Vec3& to, f32 share = 1.0f);
    /** The blocking pass over `mover`'s step of `seconds` from `from` to `to`: the first in
     * seat order it runs into at kBlockShare of its width trims `to` toward the point out of
     * them, per axis as PlayerCollideWalls trims (pmotion.c 3262-3302). Returns the push
     * velocity, units a second, that the trim leaves the mover; nobody is shoved. */
    static Vec3 block(std::span<const PlayerRuntime> players, usize mover, const Vec3& from,
                      Vec3& to, f32 seconds);
    /** Meets `mover`'s step of `seconds` from `from` to `meant`, as it was meant: whoever it
     * runs into is shoved by it, unless they stand waiting on an exit (pmotion.c 1166), and
     * the mover is carried from where it stands by the push out of them. Returns the
     * contacted party index. */
    static std::optional<usize> step(std::span<PlayerRuntime> players, usize mover,
                                     const Vec3& from, const Vec3& meant, f32 seconds,
                                     const WorldCollision* collision = nullptr);
};

} // namespace gdl::game
