#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/Enemies.h"

namespace gdl::game {
/** A constant-radius, directional damage effect attached to a creature's root.
 * Its lifetime is independent of the move that created it and of visible artwork. */
struct CritterArea {
    Mat4 local{1.0f};
    f32 radius = 0.0f;
    f32 minDot = -1.0f;
    f32 damage = 0.0f;
    u32 flags = 0;
    f32 secondsLeft = 0.0f;

    /** The effect's translation is not rotated by its own directional angles. */
    static Mat4 placement(const Mat4& parent, const Vec3& offset, const Vec2& pitchYaw);
    bool touches(const Mat4& parent, const EnemyView& player) const;
    f32 hitGap() const;
};
} // namespace gdl::game
