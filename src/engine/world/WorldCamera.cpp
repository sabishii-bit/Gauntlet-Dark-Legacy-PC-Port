#include "engine/world/WorldCamera.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl {

namespace {

/**
 * The camera's axes in world space. The original's world is left-handed: x right, y up and
 * z into the screen, so the axes form a left-handed frame and the eye looks along +z. A
 * positive pitch looks down.
 */
struct Axes {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

Axes axesOf(f32 pitch, f32 yaw, f32 roll) {
    const f32 cp = std::cos(pitch);
    const f32 sp = std::sin(pitch);
    const f32 cy = std::cos(yaw);
    const f32 sy = std::sin(yaw);
    const f32 cr = std::cos(roll);
    const f32 sr = std::sin(roll);
    Axes axes{};
    axes.forward = Vec3{sy * cp, -sp, cy * cp};
    const Vec3 up{sy * sp, cp, cy * sp};
    const Vec3 right{cy, 0.0f, -sy};
    // Roll turns right and up about the forward axis.
    axes.right = right * cr + up * sr;
    axes.up = up * cr - right * sr;
    return axes;
}

} // namespace

Vec3 WorldCamera::right() const {
    return axesOf(pitch, yaw, roll).right;
}

Vec3 WorldCamera::up() const {
    return axesOf(pitch, yaw, roll).up;
}

Vec3 WorldCamera::forward() const {
    return axesOf(pitch, yaw, roll).forward;
}

Mat4 WorldCamera::view() const {
    const Axes axes = axesOf(pitch, yaw, roll);
    // Rows are the camera axes, so eye space keeps the world's handedness: z is depth.
    Mat4 view{1.0f};
    view[0] = Vec4{axes.right.x, axes.up.x, axes.forward.x, 0.0f};
    view[1] = Vec4{axes.right.y, axes.up.y, axes.forward.y, 0.0f};
    view[2] = Vec4{axes.right.z, axes.up.z, axes.forward.z, 0.0f};
    view[3] = Vec4{-glm::dot(axes.right, position), -glm::dot(axes.up, position),
                   -glm::dot(axes.forward, position), 1.0f};
    return view;
}

Mat4 WorldCamera::projection(f32 horizontalFov, f32 aspect) {
    const f32 verticalFov = 2.0f * std::atan(std::tan(horizontalFov / 2.0f) / aspect);
    // Swapping the planes reverses depth so that nearer is larger; then it is squeezed under
    // the 2D layers.
    const f32 reversedNear = kFar;
    const f32 reversedFar = kNear;
    const Mat4 reversed = glm::perspectiveLH_ZO(verticalFov, aspect, reversedNear, reversedFar);
    return glm::scale(Mat4{1.0f}, Vec3{1.0f, 1.0f, kDepthRange}) * reversed;
}

Mat4 WorldCamera::frameMapping(f32 frameWidth, f32 frameHeight) {
    const Mat4 mapping =
        glm::translate(Mat4{1.0f}, Vec3{frameWidth / 2.0f, frameHeight / 2.0f, 0.0f});
    return glm::scale(mapping, Vec3{frameWidth / 2.0f, -frameHeight / 2.0f, 1.0f});
}

Mat4 WorldCamera::clipTransform(f32 horizontalFov, f32 frameWidth, f32 frameHeight,
                                const Mat4& frameProjection) const {
    return frameProjection * frameMapping(frameWidth, frameHeight) *
           projection(horizontalFov, frameWidth / frameHeight) * view();
}

CameraFrame CameraFrame::of(const WorldCamera& camera) {
    CameraFrame frame;
    frame.position = camera.position;
    frame.right = camera.right();
    frame.up = camera.up();
    frame.forward = camera.forward();
    return frame;
}

CameraFrame CameraFrame::at(const Vec3& eye) {
    CameraFrame frame;
    frame.position = eye;
    return frame;
}

Mat4 CameraFrame::face(const Mat4& placement, u32 mode) const {
    if (mode == 0) {
        return placement;
    }
    Mat4 faced = placement;
    const Vec3 at{placement[3]};
    if (mode == kFacingTop) {
        // An XZ ribbon keeps its length and direction. Only its width rolls to
        // put the top surface toward the eye; facing its z at the eye collapses
        // it edge-on and destroys the directions of a radial lightning burst.
        const Vec3 along{placement[2]};
        const Vec3 side = glm::cross(position - at, along);
        constexpr f32 kTopFaceThreshold = 0.01f;
        const f32 length = glm::length(side);
        if (length >= kTopFaceThreshold) {
            faced[0] = Vec4{side / length, 0.0f};
            faced[1] = Vec4{glm::cross(along, Vec3{faced[0]}), 0.0f};
        } else {
            // The original's collinear-eye fallback, also safe for a collapsed
            // animation key: no normalization of a zero-length axis.
            faced[1] = Vec4{0, 1, 0, 0};
            faced[0] = Vec4{glm::cross(Vec3{0, 1, 0}, along), 0.0f};
        }
        return faced;
    }
    // Facing replaces orientation, not the authored growth of a billboard effect.
    const Vec3 scale{glm::length(Vec3{placement[0]}), glm::length(Vec3{placement[1]}),
                     glm::length(Vec3{placement[2]})};
    if (mode == kFacingFull) {
        // A proper rotation whose z points back at the camera.
        faced[0] = Vec4{-right * scale.x, 0.0f};
        faced[1] = Vec4{up * scale.y, 0.0f};
        faced[2] = Vec4{-forward * scale.z, 0.0f};
        return faced;
    }
    const Vec3 toCamera = position - at;
    if (toCamera.x * toCamera.x + toCamera.z * toCamera.z < 1e-8f) {
        return placement;
    }
    const f32 yaw = std::atan2(toCamera.x, toCamera.z);
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    faced[0] = Vec4{c * scale.x, 0.0f, -s * scale.x, 0.0f};
    faced[1] = Vec4{0.0f, scale.y, 0.0f, 0.0f};
    faced[2] = Vec4{s * scale.z, 0.0f, c * scale.z, 0.0f};
    return faced;
}

} // namespace gdl
