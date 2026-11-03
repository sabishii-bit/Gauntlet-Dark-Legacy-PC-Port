#pragma once
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/CombatArenaTarget.h"

namespace gdl::game {
struct CombatArenaActivation;
struct CombatSpew;
struct LegendEvent;

/** What a boss encounter asks of the level: its rite, its fall, its victory and its arena.
 * Answered at once; kept by nobody. */
class BossEncounterHost {
public:
    virtual ~BossEncounterHost() = default;
    /** A step of a legend item's rite, to be shown. */
    virtual void legend(const LegendEvent& event) = 0;
    /** Advances the rite under way by `seconds`. */
    virtual void advanceLegend(f32 seconds) = 0;
    /** What the dying boss throws out: its coins. */
    virtual void spew(const CombatSpew& spew) = 0;
    /** The boss has fallen at `where`. */
    virtual void fallen(const Vec3& where) = 0;
    /** Advances the victory under way by `ticks` ticks, `seconds` in all. */
    virtual void advanceVictory(s32 ticks, f32 seconds) = 0;
    /** Where the arena's effects are anchored, destroyed cover among them. */
    virtual std::vector<Mat4> arenaAnchors() const = 0;
    /** What stands in the arena for the boss to pick from. */
    virtual std::vector<CombatArenaTarget> arenaTargets() const = 0;
    /** An eruption that makes one of the arena's obstacles solid once it has risen. */
    virtual void activateArena(const CombatArenaActivation& activation) = 0;
};
} // namespace gdl::game
