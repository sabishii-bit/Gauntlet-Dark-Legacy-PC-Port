#pragma once

#include "engine/math/Math.h"

namespace gdl::game {

/** Retail LineCylinderCollide's directional movement test. Positions are collision
 * centres; radius/halfHeight already include both bodies. An overlap permits escape,
 * but a sweep from outside cannot skip a body even when its endpoint is clear.
 * On success, contact receives the nearest point on the segment to the centre,
 * not the first cylinder intersection; on failure it is left untouched. */
bool movementTouchesBody(const Vec3& from, const Vec3& to, const Vec3& centre, f32 radius,
                         f32 halfHeight, Vec3* contact = nullptr);

} // namespace gdl::game
