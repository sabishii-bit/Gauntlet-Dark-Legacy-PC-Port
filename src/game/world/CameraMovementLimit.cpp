#include "game/world/CameraMovementLimit.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
// CameraLimitPlayerDpos's normal-view window: 30 pixels at each side,
// 40 above and 20 below its 640x448 frame. Expressed in normalized screen space.
constexpr f32 kLeft = 30.0f / 640.0f;
constexpr f32 kRight = 1 - kLeft;
constexpr f32 kTop = 40.0f / 448.0f;
constexpr f32 kBottom = 1 - 20.0f / 448.0f;
constexpr f32 kNearDepth = 0.01f;

Vec2 project(const Vec3& point, const WorldCamera& camera, const CameraView& projection) {
    const Vec3 relative = point - camera.position;
    const f32 depth = glm::dot(relative, camera.forward());
    const f32 tanX = std::tan(projection.horizontalFov * 0.5f);
    const f32 tanY = tanX / projection.aspect;
    return {0.5f + glm::dot(relative, camera.right()) / (2 * depth * tanX),
            0.5f - glm::dot(relative, camera.up()) / (2 * depth * tanY)};
}
bool worsens(f32 before, f32 after, f32 low, f32 high) {
    return (after < low && after < before) || (after > high && after > before);
}
} // namespace

bool CameraMovementLimit::allows(const Vec3& before, const Vec3& after, const Vec3& attention,
                                 const WorldCamera& camera, const CameraView& projection) {
    const f32 oldDepth = glm::dot(before - camera.position, camera.forward());
    const f32 newDepth = glm::dot(after - camera.position, camera.forward());
    if (oldDepth <= kNearDepth || newDepth <= kNearDepth) {
        // Camera cuts or scripted displacement can start a body outside the view.
        // Never strand it there: permit movement back toward the followed party.
        return glm::distance(after, attention) <= glm::distance(before, attention);
    }
    const Vec2 oldScreen = project(before, camera, projection);
    const Vec2 newScreen = project(after, camera, projection);
    return !worsens(oldScreen.x, newScreen.x, kLeft, kRight) &&
           !worsens(oldScreen.y, newScreen.y, kTop, kBottom);
}
} // namespace gdl::game
