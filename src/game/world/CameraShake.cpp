#include "game/world/CameraShake.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
void CameraShake::update(s32 ticks) {
    if (active()) {
        m_ticks = std::max(-1, m_ticks - std::max(ticks, 0));
    }
}
Vec3 CameraShake::offset() const {
    if (!active()) {
        return Vec3{0};
    }
    // DoShake: 38 degrees per remaining game-clock tick, radius 0.1.
    const f32 angle = static_cast<f32>(0.6632251158444444 * m_ticks);
    return Vec3{0.1f * std::cos(angle), 0, 0.1f * std::sin(angle)};
}
WorldCamera CameraShake::apply(const WorldCamera& camera, const Vec3& attention) const {
    WorldCamera shaken = camera;
    if (active()) {
        const Vec3 direction = attention + offset() - camera.position;
        const f32 horizontal = glm::length(Vec2{direction.x, direction.z});
        if (glm::length(direction) > 0.0001f) {
            shaken.yaw = std::atan2(direction.x, direction.z);
            shaken.pitch = -std::atan2(direction.y, horizontal);
        }
    }
    return shaken;
}
} // namespace gdl::game
