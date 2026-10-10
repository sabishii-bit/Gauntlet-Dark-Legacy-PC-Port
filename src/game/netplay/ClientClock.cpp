#include "game/netplay/ClientClock.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
bool ClientClock::begin(const MatchSession& match) {
    if (match.host() || !match.context().valid() || match.context().epoch <= m_context.epoch ||
        (match.phase() != MatchSession::Phase::Loading &&
         match.phase() != MatchSession::Phase::Running)) {
        return false;
    }
    clear();
    m_context = match.context();
    return true;
}
void ClientClock::clear() {
    *this = ClientClock{};
}
std::optional<CombatSnapshot> ClientClock::sample(const MatchSession& match, f64 seconds) {
    if (match.host() || match.context() != m_context || !std::isfinite(seconds) || seconds < 0 ||
        (match.phase() != MatchSession::Phase::Running &&
         match.phase() != MatchSession::Phase::Paused)) {
        return std::nullopt;
    }
    const auto* latest = match.playback().latest();
    if (latest == nullptr) {
        return std::nullopt;
    }
    const u64 available = latest->motion.tick;
    if (!m_started) {
        m_tick = available > kDelay ? available - kDelay : 0;
        m_started = true;
    } else if (match.phase() == MatchSession::Phase::Running && seconds > 0) {
        const u64 buffered = available - m_tick;
        if (buffered > kDelay + kCatchup) {
            // A suspended window/network outage must not replay a long stale
            // scene. Rejoin the live buffer, retaining integer tick precision.
            m_tick = available - kDelay;
            m_fraction = 0;
        } else {
            // Gentle drift correction around two checkpoint intervals. Don't
            // reset to latest-delay on every arrival: that visibly stutters.
            f64 speed = 1;
            if (buffered < 3) {
                speed = 0.95;
            } else if (buffered > 9) {
                speed = 1.05;
            }
            const f64 advanced = m_fraction + std::min(seconds, 0.25) * m_context.tickRate * speed;
            const auto whole = static_cast<u64>(advanced);
            if (whole >= available - m_tick) {
                m_tick = available;
                m_fraction = 0;
            } else {
                m_tick += whole;
                m_fraction = advanced - static_cast<f64>(whole);
            }
        }
    }
    // f32 rounding near 1 must not produce a forbidden sample fraction.
    const f32 fraction = std::min(static_cast<f32>(m_fraction), std::nextafter(1.0f, 0.0f));
    return match.playback().sample(m_tick, fraction);
}
} // namespace gdl::game
