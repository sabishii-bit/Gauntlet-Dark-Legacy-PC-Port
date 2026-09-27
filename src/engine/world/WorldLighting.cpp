#include "engine/world/WorldLighting.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl {

WorldLighting WorldLighting::forLevel(f32 ambient, const Vec3& lightDirection,
                                      const Vec3& lightColor, f32 intensity) {
    WorldLighting lighting;
    lighting.ambient = Vec3{ambient, ambient, ambient};
    lighting.lightColor = lightColor * intensity;
    const f32 length = glm::length(lightDirection);
    lighting.direction = length > 0.0f ? -lightDirection / length : Vec3{0.0f, 1.0f, 0.0f};
    return lighting;
}

WorldLighting WorldLighting::forLevel(const LevelInfo& level) {
    return forLevel(level.ambient, level.lightDirection, level.lightColor, level.lightIntensity);
}

Color WorldLighting::shade(const Vec3& normal) const {
    const f32 facing = std::clamp(glm::dot(normal, glm::normalize(direction)), 0.0f, 1.0f);
    const Vec3 lit = ambient + lightColor * facing;
    return Color::fromFloats(lit.r, lit.g, lit.b);
}

Vec3 PointLight::on(const Vec3& point, const Vec3& normal) const {
    const Vec3 toward = position - point;
    const f32 facing = glm::dot(normal, toward);
    const f32 squared = glm::dot(toward, toward);
    const f32 falloff = 1.0f - (squared / (radius * radius));
    if (facing <= 0.0f || falloff <= 0.0f || squared <= 0.0f) {
        return Vec3{0.0f};
    }
    return color * std::min(1.0f, intensity * falloff * facing / squared);
}

Color WorldLighting::shade(const Vec3& position, const Vec3& normal) const {
    const f32 facing = std::clamp(glm::dot(normal, glm::normalize(direction)), 0.0f, 1.0f);
    Vec3 lit = ambient + lightColor * facing;
    for (const PointLight& light : points) {
        lit += light.on(position, normal);
    }
    return Color::fromFloats(lit.r, lit.g, lit.b);
}

Color WorldLighting::brighten(Color base, const Vec3& position, const Vec3& normal) const {
    constexpr f32 kByte = 255.0f;
    Vec3 lit{static_cast<f32>(base.r) / kByte, static_cast<f32>(base.g) / kByte,
             static_cast<f32>(base.b) / kByte};
    for (const PointLight& light : points) {
        lit += light.on(position, normal);
    }
    Color out = Color::fromFloats(lit.r, lit.g, lit.b);
    out.a = base.a;
    return out;
}

bool WorldLighting::pointsReach(const Vec3& centre, f32 margin) const {
    return std::ranges::any_of(points, [&](const PointLight& light) {
        return glm::distance(light.position, centre) < light.radius + margin;
    });
}

} // namespace gdl
