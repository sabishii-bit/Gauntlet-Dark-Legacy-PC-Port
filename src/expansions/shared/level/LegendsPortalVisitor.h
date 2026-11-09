#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** A party member's body as the native fixtures of either platform sense it:
 * exits, key doors, traps, pressure plates, barrel blasts and falling scenery. */
struct LegendsPortalVisitor {
    u8 player = 0;
    Vec3 position{0}; ///< feet position, not the camera follow point
    bool alive = true;
    f32 radius = .75f;
    f32 height = 3;
    s32 floorObject = -1;    ///< selected native collision owner, for pressure-bridge support
    bool levitating = false; ///< Dreamcast trap contact excludes Levitation before sound/cooldown
};

} // namespace gdl::game
