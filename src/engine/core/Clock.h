#pragma once

#include <chrono>

#include "engine/core/Types.h"

namespace gdl {

/** Accumulates render time into whole simulation ticks, retaining fractional time. */
class UpdateClock {
public:
    u32 advance(f64 seconds, u32 rate);
    /** Fraction of the next simulation tick, for rendering between completed states. */
    f32 fraction(u32 rate) const;

private:
    f64 m_remainder = 0.0;
};

/** Wall-clock frame timer. Call tick() once per frame. */
class FrameClock {
public:
    FrameClock();

    /** Advances the clock; the delta is clamped to the configured maximum. */
    void tick();

    f64 deltaSeconds() const { return m_delta; }
    f64 totalSeconds() const { return m_total; }
    u64 frameIndex() const { return m_frame; }

    void setMaxDeltaSeconds(f64 seconds) { m_maxDelta = seconds; }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_start;
    Clock::time_point m_last;
    f64 m_delta = 0.0;
    f64 m_total = 0.0;
    f64 m_maxDelta = 0.25;
    u64 m_frame = 0;
};

} // namespace gdl
