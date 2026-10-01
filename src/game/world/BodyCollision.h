#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** Swept horizontal movement against the same upright volumes used for hit tests.
 * Does not move the obstacles or retain the supplied view. */
class BodyCollision {
public:
    static Vec3 resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                        std::span<const MissileTarget> bodies);
    /** Slide against authored item boxes, subdividing long steps to prevent tunnelling. */
    static Vec3 resolveItems(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                             std::span<const Obstacle> items);
};
} // namespace gdl::game
