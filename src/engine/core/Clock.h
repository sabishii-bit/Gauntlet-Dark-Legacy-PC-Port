#pragma once

#include <chrono>
#include <cstdint>

namespace gdl {

/** Wall-clock frame timer. Call tick() once per frame. */
class FrameClock {
public:
    FrameClock();

    /** Advances the clock; the delta is clamped to the configured maximum. */
    void tick();

    double deltaSeconds() const { return m_delta; }
    double totalSeconds() const { return m_total; }
    std::uint64_t frameIndex() const { return m_frame; }

    void setMaxDeltaSeconds(double seconds) { m_maxDelta = seconds; }

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_start;
    Clock::time_point m_last;
    double m_delta = 0.0;
    double m_total = 0.0;
    double m_maxDelta = 0.25;
    std::uint64_t m_frame = 0;
};

} // namespace gdl
