#pragma once
#include <vector>

#include "engine/core/Types.h"

#include "game/combat/HurtKind.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/players/PlayerImpact.h"

namespace gdl::game {
/** What the opponents report into as they act: harm, rewards, lessons, scenery bursts and
 * the camera's shake. Answered at once; kept by nobody. */
class OpponentHost {
public:
    virtual ~OpponentHost() = default;
    /** Harm to party member `member`; `directed` is whether it came from a direction. */
    virtual void hurtMember(usize member, f32 damage, HurtKind kind, bool directed,
                            const PlayerImpact& impact) = 0;
    /** Lets the blasts set off so far this tick run their step. */
    virtual void settleBlasts() = 0;
    /** A burst step against the scenery; appends what it reached to `reached`. It must not
     * start another of the swarm's blasts. */
    virtual void blastScenery(const PickupBlastReach& reach, std::vector<s32>& reached) = 0;
    /** Experience `amount` won by the player at input slot `player`; `kill` when it slew. */
    virtual void award(s32 player, s32 amount, bool kill) = 0;
    /** A generator destroyed by the player at input slot `player`. */
    virtual void destroyedGenerator(s32 player) = 0;
    /** Answers the levels the party's experience has reached. */
    virtual void updateLevels() = 0;
    /** Whether lesson `id` went up over party member `member`. */
    virtual bool lesson(s32 id, usize member) = 0;
    /** A blow that shakes the camera. */
    virtual void shake() = 0;
};
} // namespace gdl::game
