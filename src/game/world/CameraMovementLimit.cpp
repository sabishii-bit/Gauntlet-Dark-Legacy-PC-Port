#include "game/world/CameraMovementLimit.h"

#include <array>
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

Vec3 CameraMovementLimit::constrain(const Vec3& before, const Vec3& after, const Vec3& attention,
                                    const WorldCamera& camera, const CameraView& projection,
                                    const Vec3& anchorOffset) {
    const Vec3 forward = camera.forward();
    if (glm::dot(before - camera.position, forward) <= kNearDepth ||
        glm::dot(after - camera.position, forward) <= kNearDepth) {
        return glm::distance(after, attention) <= glm::distance(before, attention)
                   ? after
                   : Vec3{before.x, after.y, before.z};
    }
    const f32 tanX = std::tan(projection.horizontalFov * 0.5f);
    const f32 tanY = tanX / projection.aspect;
    const std::array planes{-camera.right() + (2 * kLeft - 1) * tanX * forward,
                            camera.right() - (2 * kRight - 1) * tanX * forward,
                            camera.up() - (1 - 2 * kTop) * tanY * forward,
                            -camera.up() + (1 - 2 * kBottom) * tanY * forward};
    Vec3 step = after - before;
    for (usize i = 0; i < planes.size(); ++i) {
        const Vec3 normal = planes[i];
        const Vec3 anchor = before + step + (i == 3 ? Vec3{0} : anchorOffset);
        if (glm::dot(anchor - camera.position, normal) <= 0 || glm::dot(step, normal) <= 0) {
            continue;
        }
        const Vec3 horizontal{normal.x, 0, normal.z};
        const f32 length = glm::dot(horizontal, horizontal);
        if (length > 1.0e-8f) {
            step -= horizontal * (glm::dot(step, horizontal) / length);
        }
    }
    // At a corner, projecting onto the second edge can restore motion through the
    // first: its ground-plane tangents are not orthogonal. Retail's CamLimitPlayerDpos
    // stops combined horizontal/vertical clipping rather than escaping either edge.
    // Recheck the final step against every plane, preserving inward recovery and the
    // actor's authored vertical movement.
    for (usize i = 0; i < planes.size(); ++i) {
        const Vec3 anchor = before + step + (i == 3 ? Vec3{0} : anchorOffset);
        if (glm::dot(anchor - camera.position, planes[i]) > 0 &&
            glm::dot(step, planes[i]) > 1.0e-6f) {
            return Vec3{before.x, after.y, before.z};
        }
    }
    return before + step;
}

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
