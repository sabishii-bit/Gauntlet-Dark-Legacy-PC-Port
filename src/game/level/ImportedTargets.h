#pragma once

#include <cmath>
#include <span>

#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game {

/** Native fixture identities have their own namespace, after every ordinary
 * attack family including player-versus-player. IDs remain stable after death. */
inline constexpr s32 kImportedTargetBase = 20000;
inline constexpr s32 kImportedTargetCount = 32768;
inline constexpr s32 kImportedArenaBase = 16384;
inline constexpr s32 kImportedSceneryBase = 24576;

struct ImportedTarget {
    s32 id = -1; ///< local native runtime index, not the shared attack ID
    Vec3 base{0};
    f32 radius = 0;
    f32 height = 0;
    bool damageable = false;
    bool magicImmune = true;
    std::span<const CollisionTriangle> surface; ///< optional authored breakable faces

    bool valid() const {
        return id >= 0 && id < kImportedTargetCount && std::isfinite(base.x) &&
               std::isfinite(base.y) && std::isfinite(base.z) && std::isfinite(radius) &&
               std::isfinite(height) && radius > 0 && height > 0 && radius <= 64 &&
               height <= (surface.empty() ? 64.0f : 128.0f);
    }
};

} // namespace gdl::game
