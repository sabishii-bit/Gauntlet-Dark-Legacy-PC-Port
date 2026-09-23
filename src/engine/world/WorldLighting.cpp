#include "engine/world/WorldLighting.h"

#include <algorithm>

namespace gdl {

WorldLighting WorldLighting::forLevel(float ambient, const Vec3& lightDirection,
                                      const Vec3& lightColor, float intensity) {
    WorldLighting lighting;
    lighting.ambient = Vec3{ambient, ambient, ambient};
    lighting.lightColor = lightColor * intensity;
    const float length = glm::length(lightDirection);
    lighting.direction = length > 0.0f ? -lightDirection / length : Vec3{0.0f, 1.0f, 0.0f};
    return lighting;
}

WorldLighting WorldLighting::forLevel(const LevelInfo& level) {
    return forLevel(level.ambient, level.lightDirection, level.lightColor, level.lightIntensity);
}

Color WorldLighting::shade(const Vec3& normal) const {
    const float facing = std::clamp(glm::dot(normal, glm::normalize(direction)), 0.0f, 1.0f);
    const Vec3 lit = ambient + lightColor * facing;
    return Color::fromFloats(lit.r, lit.g, lit.b);
}

} // namespace gdl
