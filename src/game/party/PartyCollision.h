#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/party/PlayerRuntime.h"

namespace gdl::game {

/**
 * The party's members keep out of one another (PlayerCollidePlayers, pmotion.c 4035): a step
 * that runs into another standing member ends pushed out to their radii's sum from them, and
 * the one run into is shoved by the step as it was meant, once the level's items have held it
 * off but before walls or creatures stop or turn it (pmotion.c 1124-1175); it yields to the
 * shove slowly and shows being pushed.
 */
class PartyCollision {
public:
    static constexpr f32 kCoincident = 0.001f; ///< too near to push out from: the step is undone

    /** Keeps `mover`'s step from `from` to `to` out of the others, the nearest of those it
     * runs into deciding; returns who that was, `to` resolved. */
    static std::optional<usize> resolve(std::span<const PlayerRuntime> players, usize mover,
                                        const Vec3& from, Vec3& to);
    /** Meets `mover`'s step of `seconds` from `from` to `meant`, as it was meant: whoever it
     * runs into is shoved by it, unless they stand waiting on an exit (pmotion.c 1166), and
     * the mover is carried from where it stands by the push out of them. Returns the
     * contacted party index. */
    static std::optional<usize> step(std::span<PlayerRuntime> players, usize mover,
                                     const Vec3& from, const Vec3& meant, f32 seconds,
                                     const WorldCollision* collision = nullptr);
};

} // namespace gdl::game
