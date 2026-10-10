#pragma once

#include "game/netplay/MatchSession.h"

namespace gdl::game {
/** Client presentation clock, separate from fixed-step input and host simulation.
 * Advance once per rendered frame with real elapsed time. A short snapshot buffer
 * permits fractional poses; missing state holds at the latest complete checkpoint.
 * Never extrapolates actors, changes authoritative input ticks or runs gameplay. */
class ClientClock {
public:
    static constexpr u64 kDelay = 6; // two 20 Hz checkpoints at the native 60 Hz tick rate
    static constexpr u64 kCatchup = 12;
    bool begin(const MatchSession& match);
    void clear();
    std::optional<CombatSnapshot> sample(const MatchSession& match, f64 seconds);
    u64 tick() const { return m_tick; }
    f64 fraction() const { return m_fraction; }

private:
    MatchContext m_context;
    u64 m_tick = 0;
    f64 m_fraction = 0;
    bool m_started = false;
};
} // namespace gdl::game
