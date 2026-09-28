#include "game/players/PlayerImpact.h"

#include <cmath>
#include <numbers>

namespace gdl::game {
u32 PlayerImpact::effective(f32 damage, bool braced) const {
    u32 kept = flags;
    if (braced) {
        kept = (kept & kHeavy) != 0 ? (kept & ~kHeavy) | kKnockBack : kept & ~kKnockBack;
    }
    if (damage <= 2.0f) {
        kept &= ~(kHeavy | kKnockBack);
    }
    return kept;
}

PlayerDeed PlayerImpact::reaction(f32 damage, f32 facing, bool braced) const {
    if (damage <= 0.0f) {
        return PlayerDeed::None;
    }
    const u32 effective = this->effective(damage, braced);
    if ((effective & (kKnockDown | kBlownAway | kKnockOver)) != 0) {
        const f32 away = std::atan2(direction.x, direction.z);
        const f32 delta = std::remainder(away - facing, 2.0f * std::numbers::pi_v<f32>);
        return std::abs(delta) > std::numbers::pi_v<f32> / 2.0f ? PlayerDeed::FallBack
                                                                : PlayerDeed::FallForward;
    }
    if ((effective & kKnockBack) != 0) {
        return PlayerDeed::Flinch;
    }
    if (damage > 1.0f && (effective & kSpike) != 0) {
        return PlayerDeed::Spike;
    }
    if ((effective & kStun) != 0) {
        return PlayerDeed::Reel;
    }
    return (effective & kSticky) != 0 ? PlayerDeed::Webbed : PlayerDeed::None;
}

PlayerDeed PlayerImpact::combine(PlayerDeed pending, PlayerDeed incoming) {
    const auto rank = [](PlayerDeed deed) {
        switch (deed) {
        case PlayerDeed::FallBack:
        case PlayerDeed::FallForward: return 4;
        case PlayerDeed::Flinch: return 3;
        case PlayerDeed::Spike: return 2;
        case PlayerDeed::Reel:
        case PlayerDeed::Webbed: return 1;
        default: return 0;
        }
    };
    return rank(incoming) > rank(pending) ? incoming : pending;
}
} // namespace gdl::game
