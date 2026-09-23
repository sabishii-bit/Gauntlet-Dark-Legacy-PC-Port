#pragma once
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** Release-time targeting, not lock-on or homing. Supply only live damageable
 * objects in the snapshot, never members of the party. */
class TargetAssist {
public:
    // PlayerGetTarget's normal/boss ranges and assisted facing-dot threshold.
    static constexpr f32 kRange = 30.0f;
    static constexpr f32 kBossRange = 200.0f;
    static constexpr f32 kFacingDot = 0.707f;
    static std::optional<Vec3> select(const Vec3& origin, const Vec3& facing,
                                      std::span<const MissileTarget> targets, f32 range,
                                      const WorldCollision* collision = nullptr);
    /** Pass through the selected point at unchanged horizontal speed. */
    static Vec3 velocity(const Vec3& origin, const Vec3& target, f32 speed, f32 gravity);
};
} // namespace gdl::game
