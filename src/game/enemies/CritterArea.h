#pragma once

#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/Enemies.h"

namespace gdl::game {
/** A directional damage effect attached to a creature's root or animated node.
 * Its lifetime is independent of the move that created it and of visible artwork. */
struct CritterArea {
    Mat4 local{1.0f};
    std::string node;
    f32 radius = 0.0f;
    f32 minDot = -1.0f;
    f32 damage = 0.0f;
    u32 flags = 0;
    f32 secondsLeft = 0.0f;
    f32 lifetime = 0.0f;
    bool expanding = false;

    /** The effect's translation is not rotated by its own directional angles. */
    static Mat4 placement(const Mat4& parent, const Vec3& offset, const Vec2& pitchYaw);
    bool touches(const Mat4& parent, const EnemyView& player) const;
    f32 currentRadius() const;
    f32 currentDamage() const;
    f32 hitGap() const;

private:
    f32 phase() const;
};
} // namespace gdl::game
