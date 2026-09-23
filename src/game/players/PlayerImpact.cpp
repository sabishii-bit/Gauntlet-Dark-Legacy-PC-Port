#include "game/players/PlayerImpact.h"

#include <cmath>
#include <numbers>

namespace gdl::game {
PlayerDeed PlayerImpact::reaction(f32 damage, f32 facing, bool braced) const {
    if (damage <= 0.0f) {
        return PlayerDeed::None;
    }
    u32 effective = flags;
    if (braced) {
        effective = (effective & kHeavy) != 0 ? (effective & ~kHeavy) | kKnockBack
                                              : effective & ~kKnockBack;
    }
    if (damage <= 2.0f) {
        effective &= ~(kHeavy | kKnockBack);
    }
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
    return (effective & kStun) != 0 ? PlayerDeed::Reel : PlayerDeed::None;
}

PlayerDeed PlayerImpact::combine(PlayerDeed pending, PlayerDeed incoming) {
    const auto rank = [](PlayerDeed deed) {
        switch (deed) {
        case PlayerDeed::FallBack:
        case PlayerDeed::FallForward: return 4;
        case PlayerDeed::Flinch: return 3;
        case PlayerDeed::Spike: return 2;
        case PlayerDeed::Reel: return 1;
        default: return 0;
        }
    };
    return rank(incoming) > rank(pending) ? incoming : pending;
}
} // namespace gdl::game
