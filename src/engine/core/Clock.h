#pragma once

#include <chrono>
#include <limits>

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

/** Whole fixed-rate ticks for a screen that runs whole ticks only. The part of a tick an
 * update leaves waits for the next; each owner states how much one update may bring.
 * UpdateClock is the scene clock: it keeps its remainder in seconds and also serves the
 * fraction rendering interpolates by. */
class TickAccumulator {
public:
    struct Limits {
        f64 longestUpdate = std::numeric_limits<f64>::infinity(); ///< seconds one update may bring
        s32 mostTicks = std::numeric_limits<s32>::max(); ///< ticks an update runs; more are lost
        f64 tolerance = 0.0; ///< how far under a whole tick still reaches it
    };

    TickAccumulator() = default;
    explicit TickAccumulator(const Limits& limits) : m_limits(limits) {}

    /** The whole ticks `seconds` bring at `rate` a second. A time that is negative or not
     * finite, or a rate that is not a finite positive number, brings none and keeps the carry. */
    s32 take(f64 seconds, f64 rate);
    void reset() { m_carry = 0.0; }
    f64 carry() const { return m_carry; } ///< the part of a tick not yet taken

private:
    Limits m_limits;
    f64 m_carry = 0.0;
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
