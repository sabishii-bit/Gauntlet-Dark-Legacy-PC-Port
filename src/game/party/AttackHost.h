#pragma once
#include "engine/core/Types.h"

#include "game/combat/HurtKind.h"
#include "game/players/PlayerImpact.h"

namespace gdl::game {
/** What the party's attacks report besides the fixtures: one player's blow on another, and
 * hits that rumble. Answered at once; kept by nobody. */
class AttackHost {
public:
    virtual ~AttackHost() = default;
    /** A player's blow or burn on party member `member`; always a directed hurt. */
    virtual void hurtByAttack(usize member, f32 damage, HurtKind kind,
                              const PlayerImpact& impact) = 0;
    /** An accepted melee blow on a creature, by its attacker's input slot. */
    virtual void meleeHit(s32 seat) = 0;
};
} // namespace gdl::game
