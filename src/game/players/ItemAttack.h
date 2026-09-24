#pragma once

#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {
/** PlayerMotion's item attack descriptors and ProcessEffects' expanding hit volume.
 * The archive's animation duration supplies the lifetime, not a guessed cooldown. */
struct ItemAttack {
    PlayerDeed deed = PlayerDeed::None;
    std::string_view tree;
    std::string_view sound;
    f32 damage = 0;
    f32 radius = 0;
    f32 delay = 0;
    f32 minDot = -1;
    u32 flags = 0;
    s32 chargeKind = 0;
    u32 chargeMask = 0;
    bool head = false;

    static std::optional<ItemAttack> select(const PowerupEffects& worn);
    f32 damageAt(f32 elapsed, f32 lifetime) const;
    bool reaches(const Mat4& parent, const MissileTarget& target, f32 elapsed, f32 lifetime) const;
};
} // namespace gdl::game
