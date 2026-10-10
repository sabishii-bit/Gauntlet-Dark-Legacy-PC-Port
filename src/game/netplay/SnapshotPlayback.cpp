#include "game/netplay/SnapshotPlayback.h"

#include <cmath>

namespace gdl::game {
namespace {
f32 angleBetween(f32 from, f32 to, f32 t) {
    return std::remainder(from + std::remainder(to - from, kTwoPi) * t, kTwoPi);
}
} // namespace

bool SnapshotPlayback::begin(PacketTransport::Connection host, u64 epoch) {
    if (host == 0 || epoch == 0 || (m_host != 0 && (host != m_host || epoch <= m_epoch))) {
        return false;
    }
    m_host = host;
    m_epoch = epoch;
    m_history.clear();
    return true;
}

void SnapshotPlayback::clear() {
    m_host = 0;
    m_epoch = 0;
    m_history.clear();
}

SnapshotPlayback::Admission SnapshotPlayback::receive(PacketTransport::Connection sender,
                                                      std::span<const u8> bytes) {
    if (m_host == 0 || sender != m_host) {
        return Admission::WrongHost;
    }
    const auto snapshot = MotionPacket::decode(bytes);
    if (!snapshot) {
        return Admission::Invalid;
    }
    if (snapshot->epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    if (!m_history.empty() && snapshot->tick <= m_history.back().tick) {
        return Admission::Stale;
    }
    m_history.push_back(*snapshot);
    if (m_history.size() > kHistory) {
        m_history.pop_front();
    }
    return Admission::Accepted;
}

const MotionSnapshot* SnapshotPlayback::latest() const {
    return m_history.empty() ? nullptr : &m_history.back();
}

std::optional<MotionSnapshot> SnapshotPlayback::sample(u64 tick, f32 fraction) const {
    if (m_history.empty() || !std::isfinite(fraction) || fraction < 0 || fraction >= 1) {
        return std::nullopt;
    }
    if (tick < m_history.front().tick) {
        return m_history.front();
    }
    for (usize i = 1; i < m_history.size(); ++i) {
        const auto& upper = m_history[i];
        if (upper.tick <= tick) {
            continue;
        }
        const auto& lower = m_history[i - 1];
        // Subtract integer ticks first: sessions above 2^53 ticks remain precise.
        const f32 t = static_cast<f32>((static_cast<f64>(tick - lower.tick) + fraction) /
                                       static_cast<f64>(upper.tick - lower.tick));
        MotionSnapshot shown = lower;
        if (lower.cameraContinuity == upper.cameraContinuity) {
            shown.camera.position = glm::mix(lower.camera.position, upper.camera.position, t);
            shown.camera.pitch = angleBetween(lower.camera.pitch, upper.camera.pitch, t);
            shown.camera.yaw = angleBetween(lower.camera.yaw, upper.camera.yaw, t);
            shown.camera.roll = angleBetween(lower.camera.roll, upper.camera.roll, t);
            shown.horizontalFov = glm::mix(lower.horizontalFov, upper.horizontalFov, t);
            shown.aspect = glm::mix(lower.aspect, upper.aspect, t);
        }
        for (usize seat = 0; seat < shown.players.size(); ++seat) {
            const auto& from = lower.players[seat];
            const auto& to = upper.players[seat];
            if (from && to && from->grant == to->grant && from->continuity == to->continuity) {
                shown.players[seat] = SeatMotion{from->grant, from->continuity,
                                                 glm::mix(from->position, to->position, t),
                                                 angleBetween(from->yaw, to->yaw, t)};
            }
        }
        return shown;
    }
    return m_history.back();
}

} // namespace gdl::game
