#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/**
 * A camera placed in a level with the original's pitch, yaw and roll convention, in its
 * left-handed world (x right, y up, z away). Its projection keeps depth within the range
 * under the 2D layers, so screens can draw sprites over the world without clearing depth.
 */
struct WorldCamera {
    static constexpr f32 kNear = 0.25f;
    static constexpr f32 kFar = 2000.0f;
    static constexpr f32 kDepthRange = 0.45f; ///< world depths stay below the canvas' 0.5

    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 pitch = 0.0f; ///< radians, positive looks down
    f32 yaw = 0.0f;
    f32 roll = 0.0f;

    /** Camera axes in world space: right, up and the direction looked along. */
    Vec3 right() const;
    Vec3 up() const;
    Vec3 forward() const;

    /** World to eye space; eye z is the distance ahead. */
    Mat4 view() const;

    /** Perspective with reversed depth: nearer is larger, within [0, kDepthRange]. */
    static Mat4 projection(f32 horizontalFov, f32 aspect);

    /** Maps clip space onto a frame's pixel space (y down), for the letterbox projection. */
    static Mat4 frameMapping(f32 frameWidth, f32 frameHeight);

    /** Everything from world space to the frame's clip space. */
    Mat4 clipTransform(f32 horizontalFov, f32 frameWidth, f32 frameHeight,
                       const Mat4& frameProjection) const;
};

/** Where a camera stands and which way its axes point, for what must face it. */
struct CameraFrame {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};

    static CameraFrame of(const WorldCamera& camera);
    /** A frame standing at `eye` with the world's axes. */
    static CameraFrame at(const Vec3& eye);

    /** The facing modes an object's flags ask for, in their top nibble. */
    static constexpr u32 kFacingShift = 24U;
    static constexpr u32 kFacingMask = 0xFU;
    static constexpr u32 kFacingFull = 4; ///< the whole rotation follows the camera
    static u32 facingOf(u32 objectFlags) { return (objectFlags >> kFacingShift) & kFacingMask; }

    /** `placement` with its rotation turned to face this camera: fully for kFacingFull,
     * else about the vertical so its z axis points at the camera. */
    Mat4 face(const Mat4& placement, u32 mode) const;
};

} // namespace gdl
