#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/party/PlayerMissiles.h"
#include "game/players/MeleeSelection.h"

namespace gdl::game {
/** Ordered, pointer-free cylinder geometry for read-only melee queries. Targets
 * remain host observations, not independently simulated or damageable actors. */
class MeleeGeometry {
public:
    struct Target {
        s32 id = -1;
        u64 instance = 0;
        Vec3 base{0};
        f32 radius = 0;
        f32 height = 0;
        MissileTarget::Acquisition acquisition;
        s32 contactCell = 0;
        bool contact = false;
        bool requireFacing = false;
        bool low = false;
        bool blocksMovement = true;
    };
    struct Contact {
        usize index = 0;
        f32 endpointDistance = 0;
    };

    static MissileTarget body(const Target& target);
    /** Also handles native mesh targets; the caller supplies the family/height
     * low-target rule independently of the target's physical geometry. */
    static MeleeSelection::Target candidate(const Vec3& feet, f32 height,
                                            const MissileTarget& target, bool low);
    /** Targets are in native acquisition order. Contact traversal reverses that
     * order and uses grid cell as the equal-distance tie breaker. A wall-blocked
     * winner is rejected without promoting another overlapping target. */
    static std::optional<Contact> contact(std::span<const Target> targets, const Vec3& from,
                                          const Vec3& to, f32 width, f32 halfHeight,
                                          const WorldCollision* collision = nullptr);
    static MeleeSelection::Snapshot snapshot(std::span<const Target> targets, const Vec3& feet,
                                             f32 height, f32 reach, f32 range, f32 facingDot,
                                             const Vec3& followPoint, const Vec3& from,
                                             const Vec3& to,
                                             const WorldCollision* collision = nullptr);
    static std::optional<Vec3> aim(std::span<const Target> targets, const Vec3& origin,
                                   const Vec3& facing, f32 range, const WorldCollision* collision,
                                   f32 facingDot, const MeleeSelection::Snapshot& acquisition);
};
} // namespace gdl::game
