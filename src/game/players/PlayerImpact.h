#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/PlayerAnimator.h"

namespace gdl::game {
/** Damage modifiers and the direction the blow travels, separate from its pain sound. */
struct PlayerImpact {
    static constexpr u32 kKnockBack = 0x10;
    static constexpr u32 kKnockDown = 0x20;
    static constexpr u32 kBlownAway = 0x40;
    static constexpr u32 kStun = 0x80;
    static constexpr u32 kKnockOver = 0x100;
    static constexpr u32 kSpike = 0x2000;
    static constexpr u32 kWhirlwind = 0x10000;
    static constexpr u32 kSticky = 0x4000000;
    static constexpr u32 kHeavy = kKnockDown | kBlownAway | kKnockOver | kWhirlwind;

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
