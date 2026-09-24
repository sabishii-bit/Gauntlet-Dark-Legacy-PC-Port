#include "game/world/TargetAssist.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
std::optional<MissileTarget> TargetAssist::melee(const Vec3& feet, f32 height, const Vec3& facing,
                                                 std::span<const MissileTarget> targets, f32 reach,
                                                 const WorldCollision* collision) {
    std::optional<MissileTarget> nearest;
    f32 best = reach;
    for (const MissileTarget& target : targets) {
        if (target.id < 0 || target.radius <= 0 || target.height <= 0 ||
            target.base.y >= feet.y + height || target.base.y + target.height <= feet.y) {
            continue;
        }
        const f32 distance =
            std::hypot(target.base.x - feet.x, target.base.z - feet.z) - target.radius;
        if (distance >= best) {
            continue;
        }
        const Vec3 origin{
            feet.x,
            std::clamp(feet.y + height * 0.5f, target.base.y, target.base.y + target.height),
            feet.z};
        // At an overlapping centre there is no bearing to normalize. Contact is
        // already established; do not turn a close attack into a ranged throw.
        const bool coincident = std::hypot(target.base.x - feet.x, target.base.z - feet.z) < 1e-5f;
        if (!coincident &&
            !select(origin, facing, std::span{&target, 1}, kBossRange, collision).has_value()) {
            continue;
        }
        nearest = target;
        best = distance;
    }
    return nearest;
}

std::optional<Vec3> TargetAssist::select(const Vec3& origin, const Vec3& facing,
                                         std::span<const MissileTarget> targets, f32 range,
                                         const WorldCollision* collision) {
    const f32 facingLength = std::hypot(facing.x, facing.z);
    if (facingLength < 1e-5f || range <= 0.0f) {
        return std::nullopt;
    }
    std::optional<Vec3> best;
    f32 bestDistance = range;
    for (const MissileTarget& target : targets) {
        if (target.id < 0 || target.radius <= 0.0f || target.height <= 0.0f) {
            continue;
        }
        const Vec3 point = target.base + Vec3{0, target.height * 0.5f, 0};
        const Vec3 offset = point - origin;
        const f32 flat = std::hypot(offset.x, offset.z);
        const f32 distance = glm::length(offset) - target.radius;
        if (flat < 1e-5f || distance >= bestDistance ||
            (offset.x * facing.x + offset.z * facing.z) / (flat * facingLength) < kFacingDot) {
            continue;
        }
        // Use missile collision geometry up to the target's near surface: its
        // own world geometry must not hide it from selection.
        bool blocked = false;
        if (collision != nullptr) {
            constexpr f32 kProbeRadius = 0.25f;
            const f32 length = glm::length(offset);
            const Vec3 direction = offset / length;
            const auto steps = static_cast<s32>(std::ceil((length - target.radius) / kProbeRadius));
            for (s32 step = 1; step < steps; ++step) {
                const f32 along = static_cast<f32>(step) * kProbeRadius;
                const Vec3 at = origin + direction * along;
                if (glm::distance(collision->resolveWalls(at, kProbeRadius, at.y - kProbeRadius,
                                                          at.y + kProbeRadius),
                                  at) > 1e-4f) {
                    blocked = true;
                    break;
                }
            }
        }
        if (!blocked) {
            best = point;
            bestDistance = distance;
        }
    }
    return best;
}

Vec3 TargetAssist::velocity(const Vec3& origin, const Vec3& target, f32 speed, f32 gravity) {
    const Vec3 offset = target - origin;
    const f32 flight = std::hypot(offset.x, offset.z) / std::max(speed, 1e-5f);
    if (flight < 1e-5f) {
        return Vec3{0};
    }
    return Vec3{offset.x / flight, offset.y / flight + 0.5f * gravity * flight, offset.z / flight};
}
} // namespace gdl::game
