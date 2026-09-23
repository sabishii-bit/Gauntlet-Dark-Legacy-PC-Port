#pragma once

#include <cstdint>
#include <optional>

#include "engine/math/Math.h"

#include "game/enemies/CritterData.h"

namespace gdl::game {
/** A launch from an animated attack node. The owning CritterData outlives the shot. */
struct CritterShot {
    const CritterData* data = nullptr;
    std::int32_t critter = -1;
    std::int32_t damageIndex = -1;
    Vec3 origin{0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    std::optional<Vec3> target;
    float rate = 1.0f;
    float scale = 1.0f;
    float damageScale = 1.0f;
    float birthLife = 0.0f; ///< a legend-item curb can shorten a projectile's first effect
    char realm = 'G';
};

/** Launch math shared by the boss projectile attacks (retail 0x8003d0a4 / 0x80030ae8).
 * The ballistic branch fixes horizontal speed, not the magnitude of the 3D velocity. */
struct CritterProjectile {
    static constexpr std::uint16_t kAimAtPlayer = 1;
    static constexpr std::uint16_t kBodyForward = 4;
    static constexpr std::uint16_t kStraight = 8;
    static constexpr std::uint16_t kIgnoreWorld = 0x40;
    static constexpr std::uint16_t kWaitForMorph = 0x800;
    static constexpr std::uint16_t kNoPlayerDamage = 0x1000;

    static float speed(const CritterDamage& damage, float rate);
    /** spreadSample lies in [-1, 1]; callers supply randomness, making the math testable. */
    static Vec3 velocity(const CritterDamage& damage, const CritterShot& shot,
                         float spreadSample = 0.0f);
    /** Fraction along a sweep that first enters the player's expanded vertical cylinder. */
    static std::optional<float> contact(const Vec3& from, const Vec3& to, float radius,
                                        const Vec3& feet, float playerRadius, float height);
};
} // namespace gdl::game
