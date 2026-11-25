#include "game/critters/CritterArea.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/PortableMath.h"

#include "game/combat/DamageTypes.h"
#include "game/combat/EffectFade.h"

namespace gdl::game {
namespace {
constexpr f32 kSpreadSlack = 0.066667f; ///< a spreading effect's age outlasts it by this
constexpr f32 kSteadyAge = 1.0f;        ///< a steady effect's (0x10) age is never longer
constexpr f32 kPlayerConeBand = 0.3f;   ///< the players' cone relaxes inside this of the reach
constexpr f32 kEnemyConeBand = 0.2f;    ///< the swarm's cone relaxes inside this of the reach
constexpr f32 kConeRelax = 0.85f;       ///< to this of its least dot
} // namespace
Mat4 CritterArea::placement(const Mat4& parent, const Vec3& offset, const Vec2& pitchYaw) {
    Mat4 local = portable::rotate(Mat4{1}, pitchYaw.x, Vec3{1, 0, 0});
    local = portable::rotate(local, pitchYaw.y, Vec3{0, 1, 0});
    local[3] = Vec4{offset, 1};
    return parent * local;
}

Mat4 CritterArea::transform(const Mat4& parent) const {
    Mat4 world = parent * local;
    world[3] += Vec4{displacement, 0};
    return world;
}

bool CritterArea::touches(const Mat4& parent, const EnemyView& player) const {
    const f32 activeRadius = currentRadius();
    if (player.hidden || !player.damageable || secondsLeft <= 0 || activeRadius <= 0) {
        return false;
    }
    const Mat4 world = transform(parent);
    const Vec3 delta = player.position + Vec3{0, player.height * 0.5f, 0} - Vec3{world[3]};
    const f32 distance = glm::length(Vec2{delta.x, delta.z});
    // ProcessEffects widens the reach by col_radius, the class's whole width (0x80095620).
    const f32 reach = activeRadius + player.width();
    if (std::abs(delta.y) > player.height * 0.5f + (halfHeight > 0 ? halfHeight : activeRadius) ||
        distance > reach) {
        return false;
    }
    if (minDot <= -1.0f) {
        return true;
    }
    Vec2 forward{world[2].x, world[2].z};
    const f32 length = glm::length(forward);
    forward = length > 0 ? forward / length : Vec2{0};
    const Vec2 direction = distance > 0 ? Vec2{delta.x, delta.z} / distance : Vec2{0};
    const f32 threshold = distance < kPlayerConeBand * reach ? minDot * kConeRelax : minDot;
    return glm::dot(direction, forward) >= threshold;
}

bool CritterArea::meetsEnemy(const Mat4& parent, const Vec3& upright, f32 bodyRadius) const {
    const f32 activeRadius = currentRadius();
    if (activeRadius <= 0) {
        return false;
    }
    const Mat4 world = transform(parent);
    const Vec3 delta = upright - Vec3{world[3]};
    const f32 distance = approximateFlatLength(delta);
    const f32 reach = activeRadius + bodyRadius;
    if (distance > reach) {
        return false;
    }
    if (minDot <= -1.0f) {
        return true;
    }
    // NormalVector2D lays both flat at its fast length's reciprocal, so the dot is near a cosine.
    const Vec3 direction = approximateFlatUnit(delta);
    const Vec3 forward = approximateFlatUnit(Vec3{world[2]});
    const f32 threshold = distance < kEnemyConeBand * reach ? minDot * kConeRelax : minDot;
    return direction.x * forward.x + direction.z * forward.z >= threshold;
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
    if (growFraction > 0 || shrinkFraction > 0) {
        if (lifetime <= 0) {
            return 0;
        }
        const f32 elapsed = std::clamp(1.0f - secondsLeft / lifetime, 0.0f, 1.0f);
        if (elapsed < growFraction) {
            return radius * elapsed / growFraction;
        }
        if (elapsed > 1.0f - shrinkFraction) {
            return radius * (1.0f - elapsed) / shrinkFraction;
        }
        return radius;
    }
    if (linearGrowth) {
        return lifetime > 0 ? radius * std::clamp(1.0f - secondsLeft / lifetime, 0.0f, 1.0f) : 0;
    }
    return phase() > 0.33f ? radius * (1.33f - phase()) : 0;
}

f32 CritterArea::currentDamage() const {
    if (secondsLeft <= 0) {
        return 0;
    }
    if (!expanding || linearGrowth || growFraction > 0 || shrinkFraction > 0) {
        return damage;
    }
    return phase() > 0.33f ? damage * (1.5f * (phase() - 0.33f)) : 0;
}

f32 CritterArea::hitGap() const {
    constexpr u32 kHalfSecondHits = damage::kGas;
    if ((flags & kHalfSecondHits) != 0) {
        return 0.5f;
    }
    return currentDamage() > 2 ? age() : 0.0f;
}

f32 CritterArea::fade() const {
    // Its effect carries 0x20 (CritterDoTexmodNode, or the landing's 0x28): a spreading one's.
    return effect_fade::spreading(age(), (flags & damage::kSuperShot) != 0);
}

f32 CritterArea::age() const {
    return expanding ? secondsLeft + kSpreadSlack : std::min(kSteadyAge, secondsLeft);
}
} // namespace gdl::game
