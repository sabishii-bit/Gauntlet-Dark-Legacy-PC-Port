#include "game/combat/BodyContact.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

bool movementTouchesBody(const Vec3& from, const Vec3& to, const Vec3& centre, f32 radius,
                         f32 halfHeight) {
    // PointLineColl uses the nearest point on the full 3D finite segment, not a
    // horizontal projection or an analytic first intersection with a cylinder.
    const Vec3 step = to - from;
    const f32 squaredLength = glm::dot(step, step);
    const f32 fraction = squaredLength > 0
                             ? std::clamp(glm::dot(centre - from, step) / squaredLength, 0.0f, 1.0f)
                             : 0.0f;
    const Vec3 delta = from + fraction * step - centre;
    if (glm::length(Vec2{delta.x, delta.z}) > radius || std::abs(delta.y) > halfHeight) {
        return false;
    }
    const Vec2 inward{centre.x - from.x, centre.z - from.z};
    const f32 distance = glm::length(inward);
    if (distance <= radius) {
        const Vec2 horizontal{step.x, step.z};
        const f32 length = glm::length(horizontal);
        // GUNE5D 80346348/80346350: 0.001 and -0.01. Coincident bodies may
        // separate; otherwise only an outward step escapes an existing overlap.
        if (distance < 0.001f) {
            return length < 0.001f;
        }
        const Vec2 direction = length > 0 ? horizontal / length : horizontal;
        if (glm::dot(direction, inward / distance) < -0.01f) {
            return false;
        }
    }
    return true;
}

} // namespace gdl::game
