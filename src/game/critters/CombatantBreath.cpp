#include "game/critters/CombatantBreath.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {
CombatantBreath CombatantBreath::fromNode(const Mat4& node, const AttackDefinition& damage,
                                          bool worldAxisPitch) {
    Vec3 forward{node[2]};
    const f32 length = glm::length(forward);
    if (length > 0.0f) {
        forward /= length;
    }
    const f32 cy = portable::cos(damage.yaw);
    const f32 sy = portable::sin(damage.yaw);
    forward = {forward.x * cy - forward.z * sy, forward.y, forward.z * cy + forward.x * sy};
    const f32 cp = portable::cos(damage.pitch);
    const f32 sp = portable::sin(damage.pitch);
    Vec3 direction;
    if (worldAxisPitch) {
        // N64 e0887410 passes the posed forward directly through e0014400:
        // X stays fixed; the original Y/Z rotate without a second node transform.
        direction = {forward.x, forward.y * cp - forward.z * sp, forward.z * cp + forward.y * sp};
    } else {
        // GDL pitches about the horizontal perpendicular of the yawed direction.
        // Do not normalize that axis or replace it with the imported world-X rule.
        direction = {sp * forward.x * forward.y + forward.x * cp,
                     forward.y * cp - sp * (forward.x * forward.x + forward.z * forward.z),
                     sp * forward.y * forward.z + forward.z * cp};
    }
    const Vec3 origin{node * Vec4{damage.offset, 1.0f}};
    return {origin, origin + direction * damage.maxDistance};
}

bool CombatantBreath::touches(const AttackDefinition& damage, const Vec3& centre, f32 radius,
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
