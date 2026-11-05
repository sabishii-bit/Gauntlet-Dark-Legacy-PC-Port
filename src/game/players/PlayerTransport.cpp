#include "game/players/PlayerTransport.h"

#include <algorithm>

namespace gdl::game {

bool PlayerTransport::begin(const Vec3& destination) {
    if (!armed()) {
        return false;
    }
    m_destination = destination;
    m_timer = 60;
    m_needsStepOff = true;
    return true;
}

std::optional<Vec3> PlayerTransport::update(s32 ticks) {
    const s32 before = m_timer;
    // The transport clock runs twice as fast as the sixtieth-second game clock.
    m_timer = std::max(0, m_timer - std::clamp(ticks, 0, 30) * 2);
    return before >= 30 && m_timer < 30 ? std::optional{m_destination} : std::nullopt;
}

void PlayerTransport::observeContact(bool onPad) {
    if (!active() && !onPad) {
        m_needsStepOff = false;
    }
}

void PlayerTransport::clear() {
    m_timer = 0;
    m_needsStepOff = false;
}

f32 PlayerTransport::alpha() const {
    return m_timer >= 30 ? static_cast<f32>(m_timer - 30) / 30.0f
                         : 1.0f - static_cast<f32>(m_timer) / 29.0f;
}

} // namespace gdl::game
