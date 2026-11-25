#pragma once

#include <optional>
#include <string>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "game/enemies/Enemies.h"

namespace gdl::game {
/** A directional damage effect attached to a creature's root or animated node.
 * Its lifetime is independent of the move that created it and of visible artwork. */
struct CritterArea {
    Mat4 local{1.0f};
    std::string node;
    std::optional<Mat4> worldParent; ///< static arena placement, independent of the fighter
    Vec3 velocity{0};
    Vec3 displacement{0}; ///< world-space travel, independent of attachment motion
    f32 radius = 0.0f;
    f32 minDot = -1.0f;
    f32 damage = 0.0f;
    u32 flags = 0;
    f32 secondsLeft = 0.0f;
    f32 lifetime = 0.0f;
    bool expanding = false;
    bool linearGrowth = false;
    f32 halfHeight = 0;   ///< zero retains the ordinary radius-derived vertical extent
    f32 growFraction = 0; ///< optional grow/plateau/shrink envelope, fractions of elapsed life
    f32 shrinkFraction = 0;
    bool operator==(const CritterArea&) const = default;
    friend void visitNativeState(NativeStateDigest& digest, const CritterArea& state) {
        digest.fields<16>(state);
    }

    /** The effect's translation is not rotated by its own directional angles. */
    static Mat4 placement(const Mat4& parent, const Vec3& offset, const Vec2& pitchYaw);
    Mat4 transform(const Mat4& parent) const;
    bool touches(const Mat4& parent, const EnemyView& player) const;
    /** Whether one of the swarm `bodyRadius` wide, upright through `upright` (any point of it,
     * the test being flat), is within its reach as ProcessEffects' enemy pass measures it (sfx.c
     * 3025-3063): the fast flat length from its centre against its spread and the body's radius,
     * at any height, and within its cone, relaxed to 85% inside a fifth of that sum. */
    bool meetsEnemy(const Mat4& parent, const Vec3& upright, f32 bodyRadius) const;
    f32 currentRadius() const;
    f32 currentDamage() const;
    f32 hitGap() const;
    /** How long a hit over two holds one of the swarm from the world's effects (ProcessEffects'
     * fade, sfx.c 2677-2716). */
    f32 fade() const;

private:
    f32 phase() const;
    /** ProcessEffects' age of it: what is left of it and a fifteenth, or a steady one's left up
     * to a second (sfx.c 2679-2686). */
    f32 age() const;
};
} // namespace gdl::game
