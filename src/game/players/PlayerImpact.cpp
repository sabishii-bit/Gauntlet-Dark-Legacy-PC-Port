#include "game/players/PlayerImpact.h"

#include <cmath>
#include <numbers>

#include "engine/math/PortableMath.h"

namespace gdl::game {
u32 PlayerImpact::effective(f32 damage, bool braced) const {
    u32 kept = flags;
    if (braced) {
        kept = (kept & kHeavy) != 0 ? (kept & ~kHeavy) | damage::kKnockBack
                                    : kept & ~damage::kKnockBack;
    }
    if (damage <= 2.0f) {
        kept &= ~(kHeavy | damage::kKnockBack);
    }
    return kept;
}

PlayerDeed PlayerImpact::reaction(f32 damage, f32 facing, bool braced) const {
    if (damage < 0.0f) {
        return PlayerDeed::None;
    }
    const u32 effective = this->effective(damage, braced);
    // A whirlwind outranks every other knock (PlayerKnockback's first test).
    if ((effective & damage::kWhirlwind) != 0) {
        return PlayerDeed::Whirled;
    }
    if ((effective & (damage::kKnockDown | damage::kBlownAway | damage::kKnockOver)) != 0) {
        const f32 away = portable::atan2(direction.x, direction.z);
        const f32 delta = std::remainder(away - facing, 2.0f * std::numbers::pi_v<f32>);
        return std::abs(delta) > std::numbers::pi_v<f32> / 2.0f ? PlayerDeed::FallBack
                                                                : PlayerDeed::FallForward;
    }
    if ((effective & damage::kKnockBack) != 0) {
        return PlayerDeed::Flinch;
    }
    if (damage > 1.0f && (effective & damage::kSpike) != 0) {
        return PlayerDeed::Spike;
    }
    if ((effective & damage::kStun) != 0) {
        return PlayerDeed::Reel;
    }
    return (effective & damage::kSticky) != 0 ? PlayerDeed::Webbed : PlayerDeed::None;
}

PlayerDeed PlayerImpact::combine(PlayerDeed pending, PlayerDeed incoming) {
    return reactionRank(incoming) > reactionRank(pending) ? incoming : pending;
}
} // namespace gdl::game
