#pragma once

#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/**
 * A light at a point, as the original's positional lights: it adds its colour to what faces
 * it, the more the nearer and the more squarely, out to its radius.
 */
struct PointLight {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    f32 radius = 1.0f;
    f32 intensity = 2.0f; ///< how fast it saturates: the effects' 2, a player's lantern 10

    /** What it adds to a surface at `point` facing `normal`: (1 - d^2/r^2) cos / d,
     * times the intensity, at most the whole colour. */
    Vec3 on(const Vec3& point, const Vec3& normal) const;
};

/**
 * Lighting for a level the way the original shades its vertices: a constant ambient term plus
 * one directional light where a surface faces it, each channel clamped. The defaults are the
 * menu layer's look; a level supplies its own record.
 */
struct WorldLighting {
    Vec3 ambient{0.55f, 0.55f, 0.55f};
    Vec3 lightColor{0.45f, 0.45f, 0.45f};
    Vec3 direction{0.3f, 1.0f, 0.2f}; ///< towards the light
    std::vector<PointLight> points;   ///< this frame's, the newest first

    static constexpr usize kMostPoints = 12;

    /** The level's light: a grey ambient, one light of `lightColor` scaled by `intensity`,
     * and `lightDirection` the way the light travels. */
    static WorldLighting forLevel(f32 ambient, const Vec3& lightDirection, const Vec3& lightColor,
                                  f32 intensity);
    static WorldLighting forLevel(const LevelInfo& level);

    /** A surface's shade: ambient plus the light where it faces it. */
    Color shade(const Vec3& normal) const;
    /** The same, plus the point lights, for a surface at `position`. */
    Color shade(const Vec3& position, const Vec3& normal) const;
    /** `base` with the point lights added, for a colour already shaded (prelit). */
    Color brighten(Color base, const Vec3& position, const Vec3& normal) const;
    /** Whether any point light reaches within `margin` of `centre`. */
    bool pointsReach(const Vec3& centre, f32 margin) const;
};

} // namespace gdl
