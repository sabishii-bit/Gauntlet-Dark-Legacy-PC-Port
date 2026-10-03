#pragma once

#include <chrono>

#include "engine/core/Types.h"

namespace gdl {

struct DisplayBounds {
    s32 x = 0;
    s32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

/** Selects the display covering most of a window; equal overlap keeps the first display. */
class DisplayRefresh {
public:
    static constexpr u32 kFallbackRate = 60;

    explicit DisplayRefresh(DisplayBounds window) : m_window(window) {}
    void consider(DisplayBounds display, u32 refreshRate);
    u32 rate() const { return m_rate; }

private:
    DisplayBounds m_window;
    u64 m_largestOverlap = 0;
    u32 m_rate = kFallbackRate;
    bool m_found = false;
};

/** A zero request follows the monitor; a positive cap never exceeds its refresh rate. */
u32 displayFrameRate(u32 requested, u32 monitorRefreshRate);

/** Absolute render deadlines retain small sleep overshoots instead of accumulating them.
 * Corrections never shorten a frame by more than one quarter of its period; slow frames
 * and changed caps reset the schedule rather than producing catch-up bursts. */
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    Time deadline(Time frameStart, Time frameFinished, u32 rate);

private:
    Time m_deadline;
    u32 m_rate = 0;
};

} // namespace gdl
