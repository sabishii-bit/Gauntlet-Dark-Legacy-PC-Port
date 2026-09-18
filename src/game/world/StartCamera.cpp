#include "game/world/StartCamera.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

void StartCamera::start(const WorldCamera& marker, const Vec3& party) {
    m_camera = marker;
    m_camera.roll = 0.0f;
    m_attention = marker.position + m_camera.forward() * glm::distance(marker.position, party);
    m_ticks = kHoldTicks;
    m_phase = Phase::Hold;
}

bool StartCamera::update(s32 ticks, bool skip, const Vec3& position, const Vec3& attention) {
    switch (m_phase) {
    case Phase::Off:
        return false;
    case Phase::Hold:
        m_ticks = std::max(m_ticks - ticks, 0);
        if (skip && m_ticks < kSkipBelow) {
            m_ticks = 0;
        }
        if (m_ticks == 0) {
            m_phase = Phase::Ride;
        }
        return true;
    case Phase::Ride: {
        const bool there = approach(m_camera.position, position, kPositionReach, ticks);
        const bool looking = approach(m_attention, attention, kAttentionReach, ticks);
        look();
        if (there && looking) {
            m_phase = Phase::Off;
            return false;
        }
        return true;
    }
    }
    return false;
}

bool StartCamera::approach(Vec3& point, const Vec3& target, f32 reach, s32 ticks) {
    const Vec3 gap = target - point;
    const f32 distance = glm::length(gap);
    if (distance < kArrival) {
        return true;
    }
    const f32 share =
        std::min(static_cast<f32>(ticks) * kUnitsPerTick / std::min(distance, reach), 1.0f);
    point += gap * share;
    return false;
}

void StartCamera::look() {
    const Vec3 ahead = m_attention - m_camera.position;
    const f32 flat = std::sqrt(ahead.x * ahead.x + ahead.z * ahead.z);
    if (flat < 1.0e-6f && std::abs(ahead.y) < 1.0e-6f) {
        return;
    }
    m_camera.pitch = std::atan2(-ahead.y, flat);
    m_camera.yaw = std::atan2(ahead.x, ahead.z);
    m_camera.roll = 0.0f;
}

} // namespace gdl::game
