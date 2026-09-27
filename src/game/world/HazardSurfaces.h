#pragma once

#include <optional>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game {

/**
 * A level's harmful surfaces: world objects whose flags, with their parents', name a harm in
 * bits 0xF0000 (burning floors, rollers, mine carts). A body against one of them or standing
 * on it is hurt (PlayerMotion_FloorFX); what a surface drives by a trigger (0x2000000) hurts
 * only when it is also marked to (0x8000000).
 */
class HazardSurfaces {
public:
    static constexpr u32 kHarmMask = 0xF0000;
    static constexpr u32 kTriggered = 0x2000000;
    static constexpr u32 kHarmsWhenTriggered = 0x8000000;
    static constexpr f32 kReach = 0.1f; ///< how near a wall counts as against it

    /** What a surface does to whoever touches it. */
    struct Harm {
        f32 damage = 0.0f;
        u32 impact = 0;     ///< PlayerImpact flags: knocked back or down
        bool jolts = false; ///< the heaviest kinds, which the level may sound
    };
    /** A harmful surface a body is touching, and which way it throws the body. */
    struct Touch {
        s32 object = -1;
        Harm harm;
        Vec3 away{0.0f}; ///< flat, from the surface to the body; none for a floor
    };

    /** The harm the flags of an object and its parents name, if any. */
    static std::optional<Harm> harmOf(u32 flags);
    /** What the same surface does to an enemy (EnemyWorldDamage): the knocking kind only
     * burns as much as the first, and the sixth kind does nothing. */
    static std::optional<Harm> enemyHarmOf(u32 flags);

    void bind(const WorldLayout& layout);
    void clear() { m_flags.clear(); }
    /** An object's flags with its parents'. */
    u32 flagsOf(s32 object) const;
    std::optional<Harm> harmOfObject(s32 object) const;
    /** The harmful surface a body of `radius` and `height` standing at `position` is against
     * (a wall first, as the original tests it) or on. */
    std::optional<Touch> touching(const WorldCollision& collision, const Vec3& position, f32 radius,
                                  f32 height) const;
    usize harmful() const;

private:
    std::vector<u32> m_flags;
};

} // namespace gdl::game
