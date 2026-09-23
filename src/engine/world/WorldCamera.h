#pragma once

#include <cstdint>

#include "engine/math/Math.h"

namespace gdl {

/**
 * A camera placed in a level with the original's pitch, yaw and roll convention, in its
 * left-handed world (x right, y up, z away). Its projection keeps depth within the range
 * under the 2D layers, so screens can draw sprites over the world without clearing depth.
 */
struct WorldCamera {
    static constexpr float kNear = 0.25f;
    static constexpr float kFar = 2000.0f;
    static constexpr float kDepthRange = 0.45f; ///< world depths stay below the canvas' 0.5

    Vec3 position{0.0f, 0.0f, 0.0f};
    float pitch = 0.0f; ///< radians, positive looks down
    float yaw = 0.0f;
    float roll = 0.0f;

    /** Camera axes in world space: right, up and the direction looked along. */
    Vec3 right() const;
    Vec3 up() const;
    Vec3 forward() const;

    /** World to eye space; eye z is the distance ahead. */
    Mat4 view() const;

    /** Perspective with reversed depth: nearer is larger, within [0, kDepthRange]. */
    static Mat4 projection(float horizontalFov, float aspect);

    /** Maps clip space onto a frame's pixel space (y down), for the letterbox projection. */
    static Mat4 frameMapping(float frameWidth, float frameHeight);

    /** Everything from world space to the frame's clip space. */
    Mat4 clipTransform(float horizontalFov, float frameWidth, float frameHeight,
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
    static constexpr std::uint32_t kFacingShift = 24U;
    static constexpr std::uint32_t kFacingMask = 0xFU;
    static constexpr std::uint32_t kFacingFull = 4; ///< the whole rotation follows the camera
    static std::uint32_t facingOf(std::uint32_t objectFlags) {
        return (objectFlags >> kFacingShift) & kFacingMask;
    }

    /** `placement` with its rotation turned to face this camera: fully for kFacingFull,
     * else about the vertical so its z axis points at the camera. */
    Mat4 face(const Mat4& placement, std::uint32_t mode) const;
};

} // namespace gdl
