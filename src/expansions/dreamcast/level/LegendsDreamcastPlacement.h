#pragma once

#include "engine/math/Math.h"

namespace gdl::game {

/** Native Dreamcast item angles, in record order: pitch, yaw, roll.
 * This is a local-to-world placement, not the GameCube editor convention. */
Mat4 legendsDreamcastItemPlacement(const Vec3& position, const Vec3& rotation);

} // namespace gdl::game
