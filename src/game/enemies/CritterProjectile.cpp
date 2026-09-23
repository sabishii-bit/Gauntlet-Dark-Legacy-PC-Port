#include "game/enemies/CritterProjectile.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
float CritterProjectile::speed(const CritterDamage& damage, float rate) {
    return damage.speed +
           0.75f * (std::clamp(rate, 0.5f, 1.5f) - 0.5f) * (damage.maxSpeed - damage.speed);
}

Vec3 CritterProjectile::velocity(const CritterDamage& damage, const CritterShot& shot,
                                 float spreadSample) {
    const float pace = speed(damage, shot.rate);
    if (damage.speed <= 0.0f || pace <= 0.0f) {
        return Vec3{0.0f};
    }
    Vec3 direction = shot.forward;
    if ((damage.behaviorFlags & kBodyForward) == 0) {
        direction = (damage.behaviorFlags & kAimAtPlayer) != 0 && shot.target.has_value()
                        ? *shot.target - shot.origin
                        : Vec3{shot.forward.x, -0.5f, shot.forward.z};
    }
    if ((damage.behaviorFlags & kStraight) != 0) {
        const float length = glm::length(direction);
        if (length > 0.0f) {
            direction /= length;
        }
    } else {
        const float distance = glm::length(Vec2{direction.x, direction.z});
        const float inverse = distance > 0.001f ? 1.0f / distance : 1.0f;
        direction.x *= inverse;
        direction.z *= inverse;
        direction.y = direction.y * inverse + 0.5f * damage.gravity * distance / (pace * pace);
    }
    const float yaw = damage.yaw + 0.5f * std::clamp(spreadSample, -1.0f, 1.0f) * damage.yawSpread;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    direction = {direction.x * cy - direction.z * sy, direction.y,
                 direction.z * cy + direction.x * sy};
    if ((damage.behaviorFlags & kStraight) != 0) {
        const float cp = std::cos(damage.pitch);
        const float sp = std::sin(damage.pitch);
        direction = {sp * direction.x * direction.y + direction.x * cp,
                     direction.y * cp -
                         sp * (direction.x * direction.x + direction.z * direction.z),
                     sp * direction.y * direction.z + direction.z * cp};
    }
    return direction * pace;
}

std::optional<float> CritterProjectile::contact(const Vec3& from, const Vec3& to, float radius,
                                                const Vec3& feet, float playerRadius,
                                                float height) {
    const Vec3 relative = from - feet;
    const Vec3 step = to - from;
    const float reach = std::max(0.0f, radius) + std::max(0.0f, playerRadius);
    float enter = 0.0f;
    float leave = 1.0f;
    const float a = step.x * step.x + step.z * step.z;
    const float b = relative.x * step.x + relative.z * step.z;
    const float c = relative.x * relative.x + relative.z * relative.z - reach * reach;
    if (a > 0.0f) {
        const float discriminant = b * b - a * c;
        if (discriminant < 0.0f) {
            return std::nullopt;
        }
        const float root = std::sqrt(discriminant);
        enter = std::max(enter, (-b - root) / a);
        leave = std::min(leave, (-b + root) / a);
    } else if (c > 0.0f) {
        return std::nullopt;
    }
    if (step.y != 0.0f) {
        const float lower = (-radius - relative.y) / step.y;
        const float upper = (height + radius - relative.y) / step.y;
        enter = std::max(enter, std::min(lower, upper));
        leave = std::min(leave, std::max(lower, upper));
    } else if (relative.y < -radius || relative.y > height + radius) {
        return std::nullopt;
    }
    return enter <= leave ? std::optional<float>{enter} : std::nullopt;
}
} // namespace gdl::game
