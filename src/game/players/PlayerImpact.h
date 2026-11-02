#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/DamageTypes.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::game {
/** The damage word (damage::) a blow lands on a player with, and the direction it travels,
 * separate from its pain sound. */
struct PlayerImpact {
    /** The knocks that put the body down. */
    static constexpr u32 kHeavy = damage::kHeavyKnocks;

    u32 flags = 0;
    Vec3 direction{0.0f};

    /** The flags a hit of `damage` still carries against the player: guarding downgrades
     * heavy hits to a knock back and takes a knock back off; damage of at most two knocks
     * nobody anywhere. */
    u32 effective(f32 damage, bool braced) const;
    /** Selects a grounded reaction from damage that survived guarding and level scaling. */
    PlayerDeed reaction(f32 damage, f32 facing, bool braced) const;
    /** Keep a fall queued when another, lesser contact lands in the same frame. */
    static PlayerDeed combine(PlayerDeed pending, PlayerDeed incoming);
};
} // namespace gdl::game
