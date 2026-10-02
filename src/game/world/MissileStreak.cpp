#include "game/world/MissileStreak.h"

#include <algorithm>
#include <array>

namespace gdl::game {

ImmediateBatch MissileStreak::geometry(const Vec3& position, const Vec3& velocity, f32 age,
                                       f32 scale, const CameraFrame& camera) const {
    ImmediateBatch batch;
    const f32 speed = glm::length(velocity);
    if (speed <= 0 || scale <= 0) {
        return batch;
    }
    Vec3 across = glm::cross(velocity / speed, camera.forward);
    const f32 width = glm::length(across);
    // NormalVector leaves a zero vector zero when looking exactly along the shot.
    if (width > 0) {
        across *= std::max(width, 0.25f) / width;
    }
    const Vec3 tail = position - velocity * std::min(age, 0.5f * scale);
    const Vec3 head = position + velocity * (forward / 30.0f);
    const std::array points{tail + across * (0.1f * scale), head + across * (0.8f * scale),
                            head - across * (0.8f * scale), tail - across * (0.1f * scale)};
    constexpr std::array kUv{Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    batch.begin(PrimitiveTopology::QuadList);
    for (usize i = 0; i < points.size(); ++i) {
        batch.vertex(points[i], color, kUv[i]);
    }
    batch.end();
    return batch;
}

} // namespace gdl::game
