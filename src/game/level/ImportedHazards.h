#pragma once

#include <functional>
#include <vector>

#include "game/players/PlayerImpact.h"

namespace gdl::game {

/** Native scenery contacts are resolved by the common party health/reaction code. */
struct ImportedHazardHit {
    s32 player = -1;
    f32 damage = 0;
    bool pierces = false;
    bool poison = false;
    PlayerImpact impact;
    bool armorAbsorbed = false; ///< expansion already applied armor; shields/reactions still apply
};

/** Native hazards own their clocks and victim lists, not a second enemy simulation.
 * Flags here have already been translated into the common combat vocabulary. */
using ImportedAreaDamage = std::function<void(const Vec3& position, f32 radius, f32 damage,
                                              u32 flags, std::vector<s32>& opponents)>;

} // namespace gdl::game
