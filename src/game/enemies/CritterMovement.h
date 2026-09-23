#pragma once

#include <limits>
#include <numbers>
#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** Authored locomotion limits, separate from a move's speed and attack eligibility.
 * Small home territories keep anchored bosses in place; pursuing bosses have larger
 * territories and forward/charge moves. Neither kind gains attacks outside its MOVE rows. */
struct CritterMovement {
    f32 roamRadius = std::numeric_limits<f32>::infinity(); ///< TYPE +0xAC, not a speed
    f32 turnLimit = std::numbers::pi_v<f32>;               ///< either side of the initial facing
    std::optional<Vec3> home; ///< TYPE defaultPos, absent when its Y sentinel is 999
    bool squareBounds = false;
    bool initialStepBasis = false;
    bool unrestrictedTurn = false;

    Vec3 constrain(const Vec3& proposed, const Vec3& homePosition) const;
    f32 facing(f32 wanted, f32 initialYaw) const;
    /** Local translation for an authored MOVE type. Destination-driven steps need a
     * separately reconstructed waypoint; they must not become forward pursuit. */
    static Vec3 direction(s32 moveType, f32 yaw);
};

} // namespace gdl::game
