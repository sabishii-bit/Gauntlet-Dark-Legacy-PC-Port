#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/**
 * The party's members keep out of one another (PlayerCollidePlayers, pmotion.c 4035): a step
 * that runs into another standing member ends pushed out to their radii's sum from them, and
 * the one run into is shoved by the step as it was meant (pmotion.c 1157), which it yields
 * to slowly and shows by being pushed.
 */
class PartyCollision {
public:
    static constexpr f32 kCoincident = 0.001f; ///< too near to push out from: the step is undone

    /** Keeps `mover`'s step from `from` to `to` out of the others, the nearest of those it
     * runs into deciding; returns who that was, `to` resolved. */
    static std::optional<usize> resolve(std::span<const PlayerRuntime> players, usize mover,
                                        const Vec3& from, Vec3& to);
    /** Resolves `mover`'s step of `seconds` and shoves whoever it ran into by the step it
     * meant, returning the contacted party index. */
    static std::optional<usize> step(std::span<PlayerRuntime> players, usize mover,
                                     const Vec3& from, f32 seconds,
                                     const WorldCollision* collision = nullptr);
};

} // namespace gdl::game
