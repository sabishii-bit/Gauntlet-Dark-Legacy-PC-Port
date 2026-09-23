#include "game/enemies/CritterBreath.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
CritterBreath CritterBreath::fromNode(const Mat4& node, const AttackDefinition& damage) {
    Vec3 forward{node[2]};
    const f32 length = glm::length(forward);
    if (length > 0.0f) {
        forward /= length;
    }
    const f32 cy = std::cos(damage.yaw);
    const f32 sy = std::sin(damage.yaw);
    forward = {forward.x * cy - forward.z * sy, forward.y, forward.z * cy + forward.x * sy};
    const f32 cp = std::cos(damage.pitch);
    const f32 sp = std::sin(damage.pitch);
    // Pitch about the horizontal perpendicular of the yawed direction, as authored.
    // Do not substitute a world-X rotation or renormalize its horizontal axis.
    const Vec3 direction{sp * forward.x * forward.y + forward.x * cp,
                         forward.y * cp - sp * (forward.x * forward.x + forward.z * forward.z),
                         sp * forward.y * forward.z + forward.z * cp};
    const Vec3 origin{node * Vec4{damage.offset, 1.0f}};
    return {origin, origin + direction * damage.maxDistance};
}

bool CritterBreath::touches(const AttackDefinition& damage, const Vec3& centre, f32 radius,
                            f32 halfHeight) const {
    const Vec3 fromOrigin = centre - origin;
    const f32 distance = glm::length(Vec2{fromOrigin.x, fromOrigin.z});
    if (distance < damage.minDistance || distance > damage.maxDistance) {
        return false;
    }
    const Vec3 segment = end - origin;
    const f32 lengthSquared = glm::dot(segment, segment);
    const f32 t = lengthSquared > 0.0f
                      ? std::clamp(glm::dot(fromOrigin, segment) / lengthSquared, 0.0f, 1.0f)
                      : 0.0f;
    const Vec3 separation = origin + t * segment - centre;
    const f32 across = radius + damage.radius;
    const f32 vertical = halfHeight + damage.radius;
    return glm::length(separation) <= across + vertical &&
           glm::length(Vec2{separation.x, separation.z}) <= across &&
           std::abs(separation.y) <= vertical;
}
} // namespace gdl::game
