#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * A Legends HUD look's own clock: the thirtieths of a second the scene's play clock has run.
 * It follows that clock, so it stands still while play does, a rewind takes it back and a
 * replay steps it alike; a look's fades and blinks step by it.
 */
class LegendsHudClock {
public:
    /** Assumed: neither original's HUD rate was measured (the N64 steps its HUD once a rendered
     * frame, the Dreamcast by its frame counter). */
    static constexpr f32 kStepsPerSecond = 30;
    /** The most steps one advance reports, however far the play clock ran. */
    static constexpr s64 kMostCatchUp = 30;

    /** Moves to play clock `seconds`: the steps passed since the last advance, at most
     * kMostCatchUp. The first advance only sets where the clock stands, and one that finds the
     * play clock run back moves back with it; both report none. */
    s64 advance(f32 seconds);
    /** The step the clock stands at. */
    s64 step() const { return m_step; }
    /** Whether the last advance found the play clock run back. */
    bool rewound() const { return m_rewound; }

private:
    s64 m_step = 0;
    bool m_started = false;
    bool m_rewound = false;
};

} // namespace gdl::game
