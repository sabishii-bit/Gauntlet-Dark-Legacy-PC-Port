#pragma once

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/**
 * Lighting for a level the way the original shades its vertices: a constant ambient term plus
 * one directional light where a surface faces it, each channel clamped. The defaults are the
 * menu layer's look; a level supplies its own record.
 */
struct WorldLighting {
    Vec3 ambient{0.55f, 0.55f, 0.55f};
    Vec3 lightColor{0.45f, 0.45f, 0.45f};
    Vec3 direction{0.3f, 1.0f, 0.2f}; ///< towards the light

    /** The level's light: a grey ambient, one light of `lightColor` scaled by `intensity`,
     * and `lightDirection` the way the light travels. */
    static WorldLighting forLevel(f32 ambient, const Vec3& lightDirection, const Vec3& lightColor,
                                  f32 intensity);
    static WorldLighting forLevel(const LevelInfo& level);

    /** A surface's shade: ambient plus the light where it faces it. */
    Color shade(const Vec3& normal) const;
};

} // namespace gdl
