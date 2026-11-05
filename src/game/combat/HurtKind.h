#pragma once

#include "engine/core/Types.h"

#include "game/combat/DamageTypes.h"

namespace gdl::game {
/** What hurt a character, which picks how it cries out. */
enum class HurtKind : u8 {
    Blow,      ///< cries out once enough has been taken
    Burn,      ///< always cries out
    Pierce,    ///< groans
    Gas,       ///< cloud damage: gas protection and queued pain, not the poison-trap voice
    QuietBlow, ///< accumulates pain; the attacker supplies the impact sound, without a cry
    DeathDrain ///< bypasses ordinary armor without knockback or impact audio
};

/** Whether a hurt came from a direction (a blow, a blast, gas): a raised guard halves it.
 * One from no direction in particular (a trap underfoot) the guard takes whole. */
enum class HurtSource : u8 { Undirected, Directed };

/** A trap's or imported hazard's source, by its knock flags: knockback or knockdown is Directed. */
constexpr HurtSource hurtSourceOf(u32 impactFlags) {
    return (impactFlags & (damage::kKnockBack | damage::kKnockDown)) != 0 ? HurtSource::Directed
                                                                          : HurtSource::Undirected;
}
} // namespace gdl::game
