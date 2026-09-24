#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** Swept horizontal movement against the same upright volumes used for hit tests.
 * Does not move the obstacles or retain the supplied view. */
class BodyCollision {
public:
    static Vec3 resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                        std::span<const MissileTarget> bodies);
};
} // namespace gdl::game
