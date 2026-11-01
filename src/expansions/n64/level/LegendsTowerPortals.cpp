#include "expansions/n64/level/LegendsTowerPortals.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdl::game {

SceneFog legendsCathedralFog(f32 seconds) {
    const f32 time = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 2.0f) : 0;
    // Tower has no WFOG: e003a44c uses the native default start996, far300,
    // black fog. e0028bc8 projects near.3/far300 after the quarter-scaled view,
    // hence near1.2/far1200 in the port's unscaled eye-depth coordinates.
    constexpr f32 nearPlane = 1.2f;
    constexpr f32 farPlane = 1200;
    const f32 start =
        time < 2 ? static_cast<f32>(static_cast<s32>((1 - time * time / 4) * (996 - 850) + 850))
                 : 0;
    const f32 end = static_cast<f32>(static_cast<s32>(start + 6 + time / 3 * 100));
    SceneFog result;
    result.color = Vec3{std::clamp(time * 5, 0.0f, 1.0f)};
    result.bias = (1000 * farPlane / (farPlane - nearPlane) - start) / (end - start);
    result.reciprocalScale =
        -1000 * farPlane * nearPlane / ((farPlane - nearPlane) * (end - start));
    return result;
}

LegendsTowerPortalMotion::LegendsTowerPortalMotion(LegendsTowerPortalKind kind) : m_kind(kind) {
    // Tower.bin e0862fc4/e0861630 place the completion exits; e08641c4
    // selects BOSS1, while e0864914 selects encoded stage0x801 (HELL1).
    switch (kind) {
    case LegendsTowerPortalKind::Shards: m_definition = {{0, 1, -30}, 3, "boss1"}; break;
    case LegendsTowerPortalKind::Runestones: m_definition = {{1.5f, 0, 14}, 4, "hell1"}; break;
    default: throw std::invalid_argument("Unknown Legends Tower portal kind");
    }
}

void LegendsTowerPortalMotion::capturePresentation() {
    m_previousCharge = m_charge;
    m_previousFireHeight = m_fireHeight;
}

f32 LegendsTowerPortalMotion::charge(f32 alpha) const {
    return static_cast<f32>(alpha < 0 || !std::isfinite(alpha)
                                ? m_charge
                                : std::lerp(m_previousCharge, m_charge,
                                            static_cast<f64>(std::clamp(alpha, 0.0f, 1.0f))));
}

f32 LegendsTowerPortalMotion::fireHeight(f32 alpha) const {
    return static_cast<f32>(alpha < 0 || !std::isfinite(alpha)
                                ? m_fireHeight
                                : std::lerp(m_previousFireHeight, m_fireHeight,
                                            static_cast<f64>(std::clamp(alpha, 0.0f, 1.0f))));
}

bool LegendsTowerPortalMotion::advance(f32 seconds, std::span<const LegendsPortalVisitor> visitors,
                                       bool enabled) {
    if (m_departed || !std::isfinite(seconds) || seconds <= 0 || seconds > 10) {
        return false;
    }
    if (!enabled) {
        m_charge = 0;
        m_dwell = 0;
        m_occupied = 0;
        m_fireHeight = m_previousFireHeight = -40;
        m_previousCharge = 0;
        m_fireCreated = false;
        return false;
    }
    u8 living = 0;
    u8 occupied = 0;
    // Native scans four distinct player slots. Invalid or repeated identities
    // must not make a malformed caller's roster appear unanimously ready.
    bool valid = visitors.size() <= 4;
    for (const auto& visitor : visitors) {
        if (!visitor.alive) {
            continue;
        }
        if (visitor.player >= 4) {
            valid = false;
            continue;
        }
        const auto bit = static_cast<u8>(1U << visitor.player);
        if ((living & bit) != 0 || !std::isfinite(visitor.position.x) ||
            !std::isfinite(visitor.position.y) || !std::isfinite(visitor.position.z)) {
            valid = false;
        }
        living |= bit;
        const f64 x = static_cast<f64>(visitor.position.x) - m_definition.position.x;
        const f64 z = static_cast<f64>(visitor.position.z) - m_definition.position.z;
        const f64 radius = m_definition.radius;
        if (x * x + z * z < radius * radius) {
            occupied |= bit;
        }
    }
    if (!valid) {
        occupied = 0;
    }
    m_occupied = occupied;
    if (m_kind == LegendsTowerPortalKind::Runestones) {
        // The visual responds to ANY visitor, independently of unanimous
        // charging. Once created, the native tree survives leaving/re-entry.
        m_fireCreated |= occupied != 0;
        m_fireHeight =
            std::clamp(m_fireHeight + (occupied != 0 ? 80.0 : -80.0) * seconds, -40.0, 0.0);
    }
    const bool all = living != 0 && occupied == living;
    const bool charging = m_kind == LegendsTowerPortalKind::Shards ? occupied != 0 : all;

    // e08641c4 charges on ANY visitor; e0864914 requires ALL visitors.
    // Both charge by dt/2, decay by dt, then require strictly more than1.5s
    // of unanimous occupancy. Crossing full charge does not also advance
    // dwell in that tick. Leaving resets dwell, but charge decays gradually.
    if (!charging) {
        m_charge = std::max(0.0, m_charge - seconds);
        m_dwell = 0;
    } else if (m_charge < 1) {
        m_charge = std::min(1.0, m_charge + static_cast<f64>(seconds) / 2);
        m_dwell = 0;
    } else if (all) {
        m_dwell += seconds;
    } else {
        m_dwell = 0;
    }
    if (m_dwell > 1.5) {
        m_departed = true;
        return true;
    }
    return false;
}

} // namespace gdl::game
