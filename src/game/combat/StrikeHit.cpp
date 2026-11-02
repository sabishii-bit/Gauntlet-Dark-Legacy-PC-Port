#include "game/combat/StrikeHit.h"

#include <algorithm>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {
bool StrikeHit::reaches(const Vec3& position, f32 targetRadius, f32 targetHeight) const {
    if (radius <= 0 || damage <= 0) {
        return false;
    }
    Vec3 nearest = centre;
    if (swept) {
        const Vec3 travel = centre - from;
        const f32 lengthSquared = travel.x * travel.x + travel.z * travel.z;
        if (lengthSquared > 0) {
            const Vec3 toward = position - from;
            const f32 along =
                std::clamp((toward.x * travel.x + toward.z * travel.z) / lengthSquared, 0.0f, 1.0f);
            nearest = from + travel * along;
        }
    }
    const Vec3 offset = position - nearest;
    const f32 across = portable::hypot(offset.x, offset.z);
    if (across > radius + targetRadius || offset.y > radius || offset.y + targetHeight < -radius) {
        return false;
    }
    if (arc <= -1.0f || across < 1e-4f) {
        return true;
    }
    return (offset.x * facing.x + offset.z * facing.z) / across >= arc;
}
} // namespace gdl::game
