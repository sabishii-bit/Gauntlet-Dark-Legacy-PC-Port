#pragma once

#include <span>

#include "engine/core/Types.h"

namespace gdl::game {

/** One player's run of close blows, as the swarm may answer it. */
struct SeatRun {
    s32 count = 0;
    bool running = false;  ///< the run is under way
    bool departed = false; ///< the player has left the game
};

/** One opponent step, handed to rules that answer the party. */
struct SwarmStep {
    f32 seconds = 0;
    std::span<const SeatRun> runs; ///< every participant's run, in party order
};

/** How the swarm and its generators are tuned for a step. The defaults change nothing. */
struct SwarmTuning {
    f32 breedingPace = 1;   ///< a generator's countdown is divided by this
    f32 broodScale = 1;     ///< a generator keeps this many times its brood out
    f32 speedScale = 1;     ///< the swarm walks this many times as fast
    f32 attackPace = 1;     ///< its swings land this many times as fast
    f32 healthScale = 1;    ///< it is born this many times as tough (Death excepted)
    f32 frenzyReach = 0;    ///< a generator this near a seen player breeds past its brood; 0 never
    bool wholePool = false; ///< the pool may fill to its capacity rather than the level's cap
    bool operator==(const SwarmTuning&) const = default;
};

/** Rules a mechanic lays over how the swarm answers the party. Stepped once an opponent
 * step, never while time stands still; the tuning is read wherever the swarm needs it. */
class SwarmRules {
public:
    virtual ~SwarmRules() = default;
    virtual void step(const SwarmStep& step) = 0;
    virtual SwarmTuning tuning() const = 0;
};

} // namespace gdl::game
