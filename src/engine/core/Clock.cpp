#include "engine/core/Clock.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl {

f32 UpdateClock::fraction(u32 rate) const {
    return static_cast<f32>(std::clamp(m_remainder * rate, 0.0, 1.0));
}

u32 UpdateClock::advance(f64 seconds, u32 rate) {
    if (rate == 0 || !std::isfinite(seconds) || seconds < 0) {
        return 0;
    }
    // A stalled window must not enqueue seconds of catch-up simulation.
    constexpr f64 kMaxElapsed = 0.25;
    constexpr f64 kRoundingTolerance = 1.0e-9;
    m_remainder += std::min(seconds, kMaxElapsed);
    const auto ticks = static_cast<u32>(std::floor(m_remainder * rate + kRoundingTolerance));
    m_remainder = std::max(0.0, m_remainder - static_cast<f64>(ticks) / rate);
    return ticks;
}

FrameClock::FrameClock() : m_start(Clock::now()), m_last(m_start) {}

void FrameClock::tick() {
    const auto now = Clock::now();
    m_delta = std::min(std::chrono::duration<f64>(now - m_last).count(), m_maxDelta);
    m_total = std::chrono::duration<f64>(now - m_start).count();
    m_last = now;
    ++m_frame;
}

} // namespace gdl
