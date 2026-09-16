#include "engine/core/Clock.h"

#include <algorithm>

namespace gdl {

FrameClock::FrameClock() : m_start(Clock::now()), m_last(m_start) {}

void FrameClock::tick() {
    const auto now = Clock::now();
    m_delta = std::min(std::chrono::duration<f64>(now - m_last).count(), m_maxDelta);
    m_total = std::chrono::duration<f64>(now - m_start).count();
    m_last = now;
    ++m_frame;
}

} // namespace gdl
