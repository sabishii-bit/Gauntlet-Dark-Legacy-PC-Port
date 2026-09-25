#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Types.h"

#include "game/world/MoveStrikes.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
namespace {
Vec3 onEdge(const Vec3& point, const Vec3& a, const Vec3& b) {
    const Vec3 edge = b - a;
    const f32 length = glm::dot(edge, edge);
    return a + edge * (length > 0 ? std::clamp(glm::dot(point - a, edge) / length, 0.0f, 1.0f) : 0);
}

/** Project onto a face when inside it, otherwise onto its nearest edge. Degenerate
 * triangles remain usable as edges rather than dividing by a zero area. */
Vec3 onTriangle(const Vec3& point, const CollisionTriangle& triangle) {
    const auto& v = triangle.vertices;
    const Vec3 normal = glm::cross(v[1] - v[0], v[2] - v[0]);
    const f32 area = glm::dot(normal, normal);
    const Vec3 projected =
        area > 0 ? point - normal * (glm::dot(point - v[0], normal) / area) : point;
    bool inside = area > 0;
    Vec3 nearest = v[0];
    f32 best = std::numeric_limits<f32>::max();
    for (usize i = 0; i < v.size(); ++i) {
        const Vec3& a = v[i];
        const Vec3& b = v[(i + 1) % v.size()];
        inside = inside && glm::dot(glm::cross(b - a, projected - a), normal) >= 0;
        const Vec3 candidate = onEdge(point, a, b);
        const f32 distance = glm::dot(candidate - point, candidate - point);
        if (distance < best) {
            best = distance;
            nearest = candidate;
        }
    }
    return inside ? projected : nearest;
}
} // namespace

Vec3 MissileTarget::pointNear(const Vec3& point) const {
    Vec3 nearest = base + Vec3{0, height * 0.5f, 0};
    f32 best = std::numeric_limits<f32>::max();
    for (const auto& triangle : surface) {
        const Vec3 candidate = onTriangle(point, triangle);
        const f32 distance = glm::dot(candidate - point, candidate - point);
        if (distance < best) {
            best = distance;
            nearest = candidate;
        }
    }
    return nearest;
}

bool MissileTarget::touches(const Vec3& point, f32 reach) const {
    if (!surface.empty()) {
        return glm::distance(pointNear(point), point) <= reach;
    }
    return std::hypot(point.x - base.x, point.z - base.z) <= radius + reach &&
           point.y + reach >= base.y && point.y - reach <= base.y + height;
}

bool MissileTarget::reachedBy(const StrikeHit& strike) const {
    if (surface.empty()) {
        return strike.reaches(base, radius, height);
    }
    constexpr f32 kStep = 0.25f;
    const auto steps =
        strike.swept
            ? std::max(
                  1, static_cast<s32>(std::ceil(glm::distance(strike.from, strike.centre) / kStep)))
            : 1;
    for (s32 i = 0; i <= steps; ++i) {
        const Vec3 origin = strike.swept ? glm::mix(strike.from, strike.centre,
                                                    static_cast<f32>(i) / static_cast<f32>(steps))
                                         : strike.centre;
        const Vec3 point = pointNear(origin);
        if (glm::distance(origin, point) <= strike.radius && strike.reaches(point, 0, 0)) {
            return true;
        }
    }
    return false;
}
} // namespace gdl::game
