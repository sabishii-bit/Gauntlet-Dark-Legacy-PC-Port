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
    if (player.hidden || secondsLeft <= 0 || radius <= 0) {
        return false;
    }
    const Mat4 world = parent * local;
    const Vec3 delta = player.position + Vec3{0, player.height * 0.5f, 0} - Vec3{world[3]};
    const f32 distance = glm::length(Vec2{delta.x, delta.z});
    const f32 reach = radius + player.radius;
    if (std::abs(delta.y) > player.height * 0.5f + radius || distance > reach) {
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

f32 CritterArea::hitGap() const {
    constexpr u32 kHalfSecondHits = 0x800;
    if ((flags & kHalfSecondHits) != 0) {
        return 0.5f;
    }
    return damage > 2 ? std::min(1.0f, secondsLeft) : 0.0f;
}
} // namespace gdl::game
