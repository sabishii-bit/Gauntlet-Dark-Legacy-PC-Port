#include "engine/platform/DisplayTiming.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl {
namespace {
u64 overlap(s32 firstStart, u32 firstSize, s32 secondStart, u32 secondSize) {
    const s64 end = std::min(static_cast<s64>(firstStart) + firstSize,
                             static_cast<s64>(secondStart) + secondSize);
    const s64 start = std::max(firstStart, secondStart);
    return static_cast<u64>(std::max(end - start, s64{0}));
}
} // namespace

void DisplayRefresh::consider(DisplayBounds display, u32 refreshRate) {
    if (display.width == 0 || display.height == 0) {
        return;
    }
    const u64 area = overlap(m_window.x, m_window.width, display.x, display.width) *
                     overlap(m_window.y, m_window.height, display.y, display.height);
    if (!m_found || area > m_largestOverlap) {
        m_found = true;
        m_largestOverlap = area;
        m_rate = refreshRate == 0 ? kFallbackRate : refreshRate;
    }
}

u32 displayFrameRate(u32 requested, u32 monitorRefreshRate) {
    const u32 refresh =
        monitorRefreshRate == 0 ? DisplayRefresh::kFallbackRate : monitorRefreshRate;
    return requested == 0 ? refresh : std::min(requested, refresh);
}

FramePacer::Time FramePacer::deadline(Time frameStart, Time frameFinished, u32 rate) {
    rate = rate == 0 ? DisplayRefresh::kFallbackRate : rate;
    const auto period =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(1.0 / rate));
    if (rate != m_rate) {
        m_deadline = frameStart + period;
        m_rate = rate;
    } else {
        m_deadline += period;
        if (m_deadline <= frameStart) {
            m_deadline = frameStart + period;
        } else {
            m_deadline = std::max(m_deadline, frameStart + period - period / 4);
        }
    }
    // Rendering itself overran: the next frame gets a fresh period from here.
    m_deadline = std::max(m_deadline, frameFinished);
    return m_deadline;
}
} // namespace gdl
