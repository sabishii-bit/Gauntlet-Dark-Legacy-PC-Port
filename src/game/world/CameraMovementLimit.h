#pragma once

#include "engine/world/WorldCamera.h"

#include "game/world/TowerCamera.h"

namespace gdl::game {
/** Blocks voluntary movement out through the shared view, not movement back into it.
 * The safe window reserves room for bodies and HUD. It is independent of input bindings. */
class CameraMovementLimit {
public:
    /** Remove outward horizontal motion while retaining the tangent along each edge.
     * The lower edge checks feet; the other planes check the collision/follow anchor. */
    static Vec3 constrain(const Vec3& before, const Vec3& after, const Vec3& attention,
                          const WorldCamera& camera, const CameraView& projection,
                          const Vec3& anchorOffset = Vec3{0});
    static bool allows(const Vec3& before, const Vec3& after, const Vec3& attention,
                       const WorldCamera& camera, const CameraView& projection);
};
} // namespace gdl::game
