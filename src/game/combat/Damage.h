#pragma once

#include "engine/core/Types.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {
/** A hit after armour, elemental affinities and protective items have been applied: its
 * amount and its damage word (damage::), met by the armour word (armor::) the target
 * wears and the armour stat it has. */
struct Damage {
    f32 amount = 0;
    u32 flags = 0;

    /** Negative results heal. Boss encounters use their own elemental multipliers. */
    static Damage modify(f32 amount, u32 flags, u32 shield, f32 armorStat, bool bossEncounter);
};
} // namespace gdl::game
