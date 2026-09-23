#include "game/enemies/CritterArea.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
Mat4 CritterArea::placement(const Mat4& parent, const Vec3& offset, const Vec2& pitchYaw) {
    Mat4 local = glm::rotate(Mat4{1}, pitchYaw.x, Vec3{1, 0, 0});
    local = glm::rotate(local, pitchYaw.y, Vec3{0, 1, 0});
    local[3] = Vec4{offset, 1};
    return parent * local;
}

bool CritterArea::touches(const Mat4& parent, const EnemyView& player) const {
    const f32 activeRadius = currentRadius();
    if (player.hidden || secondsLeft <= 0 || activeRadius <= 0) {
        return false;
    }
    const Mat4 world = parent * local;
    const Vec3 delta = player.position + Vec3{0, player.height * 0.5f, 0} - Vec3{world[3]};
    const f32 distance = glm::length(Vec2{delta.x, delta.z});
    const f32 reach = activeRadius + player.radius;
    if (std::abs(delta.y) > player.height * 0.5f + activeRadius || distance > reach) {
        return false;
    }
    if (minDot <= -1.0f) {
        return true;
    }
    Vec2 forward{world[2].x, world[2].z};
    const f32 length = glm::length(forward);
    forward = length > 0 ? forward / length : Vec2{0};
    const Vec2 direction = distance > 0 ? Vec2{delta.x, delta.z} / distance : Vec2{0};
    const f32 threshold = distance < 0.3f * reach ? minDot * 0.85f : minDot;
    return glm::dot(direction, forward) >= threshold;
}

f32 CritterArea::phase() const {
    // ProcessEffects (0x80094be0): this is remaining life, not elapsed life.
    // The authored curve deliberately uses 0.33, not an exact third.
    return lifetime <= 1.0f / 30.0f ? 1.0f : secondsLeft / lifetime;
}

f32 CritterArea::currentRadius() const {
    if (secondsLeft <= 0) {
        return 0;
    }
    if (!expanding) {
        return radius;
    }
    return phase() > 0.33f ? radius * (1.33f - phase()) : 0;
}

f32 CritterArea::currentDamage() const {
    if (secondsLeft <= 0) {
        return 0;
    }
    if (!expanding) {
        return damage;
    }
    return phase() > 0.33f ? damage * (1.5f * (phase() - 0.33f)) : 0;
}

f32 CritterArea::hitGap() const {
    constexpr u32 kHalfSecondHits = 0x800;
    if ((flags & kHalfSecondHits) != 0) {
        return 0.5f;
    }
    const f32 gap = expanding ? secondsLeft + 0.066667f : std::min(1.0f, secondsLeft);
    return currentDamage() > 2 ? gap : 0.0f;
}
} // namespace gdl::game
