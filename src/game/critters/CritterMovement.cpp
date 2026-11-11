#include "game/critters/CritterMovement.h"

#include <algorithm>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/PortableMath.h"

#include "game/critters/CritterData.h"
#include "game/enemies/EnemyMind.h"

namespace gdl::game {

Vec3 CritterMovement::constrain(const Vec3& proposed, const Vec3& homePosition) const {
    Vec3 offset = proposed - homePosition;
    const f32 radius = std::max(roamRadius, 0.0f);
    if (squareBounds) {
        offset.x = std::clamp(offset.x, -radius, radius);
        offset.z = std::clamp(offset.z, -radius, radius);
    } else {
        // CritterTranslate measures the offset by the original's fast length (0x8003ad50;
        // DC 0x8c08a878, N64 Boss.bin 0xe0883f6c), so the territory's edge is not quite round.
        const f32 distance = approximateFlatLength(offset);
        if (distance > radius) {
            offset.x *= radius / distance;
            offset.z *= radius / distance;
        }
    }
    return homePosition + offset;
}

f32 CritterMovement::facing(f32 wanted, f32 initialYaw) const {
    if (unrestrictedTurn) {
        return wrapAngle(wanted);
    }
    const f32 limit = std::max(turnLimit, 0.0f);
    return wrapAngle(initialYaw + std::clamp(wrapAngle(wanted - initialYaw), -limit, limit));
}

Vec3 CritterMovement::direction(s32 moveType, f32 yaw) {
    const Vec3 forward{portable::sin(yaw), 0.0f, portable::cos(yaw)};
    switch (moveType) {
    case MoveDefinition::kStepLeft: return Vec3{-forward.z, 0.0f, forward.x};
    case MoveDefinition::kStepRight: return Vec3{forward.z, 0.0f, -forward.x};
    case MoveDefinition::kStepBack: return -forward;
    case MoveDefinition::kStepToPoint: return Vec3{0.0f};
    default: return forward;
    }
}

} // namespace gdl::game
