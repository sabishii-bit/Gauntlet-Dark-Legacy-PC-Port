#pragma once

#include <algorithm>
#include <span>
#include <string_view>

namespace gdl {

/** The textures the optional heat distortion shimmers over, as the caller names them:
 * colour and additive blending alone cannot tell fire from poison, lightning, portals or
 * reflected light. It views names the caller keeps alive; empty, nothing is hot. */
struct ThermalMaterial {
    std::span<const std::string_view> particles; ///< particle textures that are flames
    std::span<const std::string_view> surfaces;  ///< animated surface slots that burn

    bool particle(std::string_view name) const {
        return std::ranges::find(particles, name) != particles.end();
    }
    bool surface(std::string_view name) const {
        return std::ranges::find(surfaces, name) != surfaces.end();
    }
};
} // namespace gdl
