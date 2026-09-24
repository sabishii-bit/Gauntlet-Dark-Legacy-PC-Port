#include "game/world/BodyCollision.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
Vec3 BodyCollision::resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                            std::span<const MissileTarget> bodies) {
    Vec2 position{from.x, from.z};
    Vec2 remaining{to.x - from.x, to.z - from.z};
    // Re-sweep the slide, so resolving one body cannot push through another.
    for (s32 iteration = 0; iteration < 4; ++iteration) {
        const f32 lengthSquared = glm::dot(remaining, remaining);
        if (lengthSquared < 1e-10f) {
            break;
        }
        f32 first = 1;
        Vec2 normal{0};
        bool hit = false;
        for (const auto& body : bodies) {
            if (body.radius <= 0 || body.height <= 0 ||
                std::min(from.y, to.y) >= body.base.y + body.height ||
                std::max(from.y, to.y) + height <= body.base.y) {
                continue;
            }
            const Vec2 delta = position - Vec2{body.base.x, body.base.z};
            const f32 reach = radius + body.radius;
            const f32 separation = glm::dot(delta, delta) - reach * reach;
            const f32 toward = glm::dot(delta, remaining);
            // A spawn/animation may begin overlapping. Allow escape, not a deeper step.
            if (toward >= 0) {
                continue;
            }
            const f32 discriminant = toward * toward - lengthSquared * separation;
            if (discriminant < 0) {
                continue;
            }
            const f32 time =
                separation <= 0 ? 0 : (-toward - std::sqrt(discriminant)) / lengthSquared;
            if (time < 0 || time > first) {
                continue;
            }
            const Vec2 contact = delta + remaining * time;
            const f32 distance = glm::length(contact);
            if (distance <= 1e-5f) {
                continue;
            }
            first = time;
            normal = contact / distance;
            hit = true;
        }
        position += remaining * first;
        if (!hit) {
            break;
        }
        remaining *= 1 - first;
        remaining -= normal * std::min(glm::dot(remaining, normal), 0.0f);
    }
    return {position.x, to.y, position.y};
}
} // namespace gdl::game
