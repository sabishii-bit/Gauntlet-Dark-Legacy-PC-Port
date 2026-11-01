#include "expansions/n64/level/LegendsPortals.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
} // namespace

LegendsPortalDeparture legendsPortalDeparture(f32 seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return {};
    }
    // e004a6e8 switches on substate-4: native state0x12 is case0x0e.
    // e004e1a8 starts its elapsed clock at zero. It never moves the feet.
    const f32 elapsed = std::min(seconds, kLegendsPortalDepartureSeconds);
    return {elapsed * 9.3f, 1.0f - elapsed * .9f / kLegendsPortalDepartureSeconds,
            seconds >= kLegendsPortalDepartureSeconds};
}

bool touchesLegendsPortal(const formats::LegendsN64Portal& portal,
                          const LegendsPortalVisitor& visitor) {
    // e00638b8 shape1 stores feetY/height; e00604b4 tests inclusive vertical
    // overlap and strict XZ radius sum. Retain GDL floating-point body reach.
    if (!visitor.alive || visitor.player >= 4 || !finite(visitor.position) ||
        !finite(portal.position) || !std::isfinite(visitor.radius) || visitor.radius < 0 ||
        !std::isfinite(visitor.height) || visitor.height < 0 || !std::isfinite(portal.radius) ||
        portal.radius <= 0 || !std::isfinite(portal.height) || portal.height <= 0) {
        return false;
    }
    if (visitor.position.y > portal.position.y + portal.height ||
        visitor.position.y + visitor.height < portal.position.y) {
        return false;
    }
    const f64 x = static_cast<f64>(visitor.position.x) - portal.position.x;
    const f64 z = static_cast<f64>(visitor.position.z) - portal.position.z;
    const f64 radius = static_cast<f64>(visitor.radius) + portal.radius;
    return x * x + z * z < radius * radius;
}

LegendsPortalMotion::LegendsPortalMotion(u32 startingFrames, bool hasStarting, bool hasClosing)
    : m_startingFrames(startingFrames), m_hasStarting(hasStarting), m_hasClosing(hasClosing) {
    if (startingFrames > 65535) {
        throw std::invalid_argument("Legends portal startup exceeds the native sequence range");
    }
}

void LegendsPortalMotion::reset() {
    if (m_phase != Phase::Idle) {
        m_phase = Phase::Idle;
        ++m_generation;
    }
    m_waiting = 0;
    m_previousOccupants = 0;
    m_accumulator = 0;
}

bool LegendsPortalMotion::advance(f32 seconds, u8 occupied, u8 living, bool enabled) {
    if (!enabled) {
        reset();
        return false;
    }
    // The scene calls this with fixed gameplay ticks, never presentation time.
    // Refuse malformed/unbounded deltas instead of hanging on corrupt input.
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 10 || (occupied & 0xf0U) != 0 ||
        (living & 0xf0U) != 0 || m_phase == Phase::Departed) {
        return false;
    }
    m_accumulator += static_cast<f64>(seconds) * 30;
    bool departed = false;
    while (m_accumulator >= 1) {
        m_accumulator -= 1;
        departed |= tick(static_cast<u8>(occupied & living), living);
    }
    return departed;
}

bool LegendsPortalMotion::tick(u8 occupied, u8 living) {
    // e0068118 subtype5: initial duration is sequence frames*5+150 at 150Hz;
    // later occupancy changes restore at least300 units. Native gameplay is30Hz.
    if (m_phase == Phase::Departed) {
        return false;
    }
    if (m_phase == Phase::Idle || m_phase == Phase::Closing) {
        if (occupied != 0) {
            m_phase = m_hasStarting ? Phase::Starting : Phase::Active;
            ++m_generation;
            m_waiting = static_cast<s32>(m_startingFrames) + 30;
            m_previousOccupants = 0;
        }
        return false;
    }
    if (occupied != m_previousOccupants) {
        m_waiting = std::max(m_waiting, 60);
    }
    m_previousOccupants = occupied;
    if (m_waiting > 0) {
        --m_waiting;
        return false;
    }
    if (living != 0 && occupied == living) {
        m_phase = Phase::Departed;
        ++m_generation;
        return true;
    }
    if (occupied == 0) {
        m_phase = m_hasClosing ? Phase::Closing : Phase::Idle;
        ++m_generation;
    }
    return false;
}

void LegendsPortalMotion::finishPresentation() {
    if (m_phase == Phase::Starting) {
        m_phase = Phase::Active;
        ++m_generation;
    } else if (m_phase == Phase::Closing) {
        m_phase = Phase::Idle;
        ++m_generation;
    }
}

} // namespace gdl::game
