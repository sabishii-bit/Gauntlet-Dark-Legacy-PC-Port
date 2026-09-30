#include "game/world/CameraShake.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
void CameraShake::start(Target target, s32 lead, s32 ticks, f32 radius, s32 priority) {
    if (active() && priority < m_priority) {
        return;
    }
    m_target = target;
    m_lead = std::max(lead, 0);
    m_ticks = ticks;
    m_radius = radius;
    m_priority = priority;
}
void CameraShake::clear() {
    m_ticks = -1;
    m_lead = 0;
    m_priority = 0;
}
void CameraShake::update(s32 ticks) {
    if (active()) {
        m_ticks = std::max(-1, m_ticks - std::max(ticks, 0));
        m_lead = std::max(0, m_lead - std::max(ticks, 0));
    }
}
Vec3 CameraShake::offset() const {
    if (!active() || m_lead > 0) {
        return Vec3{0};
    }
    // DoShake: 38 degrees per remaining game-clock tick.
    const f32 angle = static_cast<f32>(0.6632251158444444 * m_ticks);
    return Vec3{m_radius * std::cos(angle), 0, m_radius * std::sin(angle)};
}
WorldCamera CameraShake::apply(const WorldCamera& camera, const Vec3& attention) const {
    WorldCamera shaken = camera;
    if (active()) {
        Vec3 aim = attention;
        if (m_target != Target::Eye) {
            aim += offset();
        }
        if (m_target != Target::Attention) {
            shaken.position += offset();
        }
        const Vec3 direction = aim - shaken.position;
        const f32 horizontal = glm::length(Vec2{direction.x, direction.z});
        if (glm::length(direction) > 0.0001f) {
            shaken.yaw = std::atan2(direction.x, direction.z);
            shaken.pitch = -std::atan2(direction.y, horizontal);
        }
    }
    return shaken;
}
} // namespace gdl::game
