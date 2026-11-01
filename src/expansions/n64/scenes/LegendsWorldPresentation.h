#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** Verified N64 drawable flags, separate from WOBJ's collision/world flags.
 * Texture alpha and deferred-model eligibility remain independent inputs. */
struct LegendsWorldPresentation {
    bool hidden = false;
    bool sortable = false;
    bool forceAlpha = false;
    bool depthWrite = true;
    bool depthTest = true;
    bool unlit = false;
    u32 facing = 0;
    bool cameraDepthPull = false;

    static LegendsWorldPresentation n64(u32 nativeDrawFlags, u32 nativeWorldFlags = 0);

    /** Mode1 follows the native view-space yaw calculation. Other modes retain
     * the authored basis until their native conventions have been recovered. */
    Mat4 placement(const Mat4& authored, const WorldCamera& camera) const;

    /** Native dynamic-decal correction applied only at submission, never to
     * simulation geometry, collision, or object-selection coordinates. */
    Mat4 drawAdjustment(const WorldCamera& camera) const;
};

} // namespace gdl::game
