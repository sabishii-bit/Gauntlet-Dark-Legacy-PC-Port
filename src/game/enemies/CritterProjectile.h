#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/CritterData.h"

namespace gdl::game {
/** A launch from an animated attack node. The owning CritterData outlives the shot. */
struct CritterShot {
    const CritterData* data = nullptr;
    s32 critter = -1;
    s32 damageIndex = -1;
    Vec3 origin{0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    std::optional<Vec3> target;
    f32 rate = 1.0f;
    f32 scale = 1.0f;
    f32 damageScale = 1.0f;
    f32 birthLife = 0.0f; ///< a legend-item curb can shorten a projectile's first effect
    char realm = 'G';
};

/** Launch math shared by the boss projectile attacks (retail 0x8003d0a4 / 0x80030ae8).
 * The ballistic branch fixes horizontal speed, not the magnitude of the 3D velocity. */
struct CritterProjectile {
    static constexpr u16 kAimAtPlayer = 1;
    static constexpr u16 kBodyForward = 4;
    static constexpr u16 kStraight = 8;
    static constexpr u16 kIgnoreWorld = 0x40;
    static constexpr u16 kWaitForMorph = 0x800;
    static constexpr u16 kNoPlayerDamage = 0x1000;

    static f32 speed(const CritterDamage& damage, f32 rate);
    /** spreadSample lies in [-1, 1]; callers supply randomness, making the math testable. */
    static Vec3 velocity(const CritterDamage& damage, const CritterShot& shot,
                         f32 spreadSample = 0.0f);
    /** Fraction along a sweep that first enters the player's expanded vertical cylinder. */
    static std::optional<f32> contact(const Vec3& from, const Vec3& to, f32 radius,
                                      const Vec3& feet, f32 playerRadius, f32 height);
};
} // namespace gdl::game
