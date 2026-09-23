#include "game/enemies/CritterMovement.h"

#include <algorithm>
#include <cmath>

#include "game/enemies/CritterData.h"
#include "game/enemies/EnemyMind.h"

namespace gdl::game {

Vec3 CritterMovement::constrain(const Vec3& proposed, const Vec3& homePosition) const {
    Vec3 offset = proposed - homePosition;
    const float radius = std::max(roamRadius, 0.0f);
    if (squareBounds) {
        offset.x = std::clamp(offset.x, -radius, radius);
        offset.z = std::clamp(offset.z, -radius, radius);
    } else {
        const float distance = glm::length(Vec2{offset.x, offset.z});
        if (distance > radius) {
            offset.x *= radius / distance;
            offset.z *= radius / distance;
        }
    }
    return homePosition + offset;
}

float CritterMovement::facing(float wanted, float initialYaw) const {
    if (unrestrictedTurn) {
        return wrapAngle(wanted);
    }
    const float limit = std::max(turnLimit, 0.0f);
    return wrapAngle(initialYaw + std::clamp(wrapAngle(wanted - initialYaw), -limit, limit));
}

Vec3 CritterMovement::direction(int moveType, float yaw) {
    const Vec3 forward{std::sin(yaw), 0.0f, std::cos(yaw)};
    switch (moveType) {
    case CritterMove::kStepLeft: return Vec3{-forward.z, 0.0f, forward.x};
    case CritterMove::kStepRight: return Vec3{forward.z, 0.0f, -forward.x};
    case CritterMove::kStepBack: return -forward;
    case CritterMove::kStepToPoint: return Vec3{0.0f};
    default: return forward;
    }
}

} // namespace gdl::game
