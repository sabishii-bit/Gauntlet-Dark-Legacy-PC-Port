#include "engine/world/WorldCamera.h"

#include <cmath>
#include <cstdint>

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

Axes axesOf(float pitch, float yaw, float roll) {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
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

Mat4 WorldCamera::projection(float horizontalFov, float aspect) {
    const float verticalFov = 2.0f * std::atan(std::tan(horizontalFov / 2.0f) / aspect);
    // Swapping the planes reverses depth so that nearer is larger; then it is squeezed under
    // the 2D layers.
    const float reversedNear = kFar;
    const float reversedFar = kNear;
    const Mat4 reversed = glm::perspectiveLH_ZO(verticalFov, aspect, reversedNear, reversedFar);
    return glm::scale(Mat4{1.0f}, Vec3{1.0f, 1.0f, kDepthRange}) * reversed;
}

Mat4 WorldCamera::frameMapping(float frameWidth, float frameHeight) {
    const Mat4 mapping =
        glm::translate(Mat4{1.0f}, Vec3{frameWidth / 2.0f, frameHeight / 2.0f, 0.0f});
    return glm::scale(mapping, Vec3{frameWidth / 2.0f, -frameHeight / 2.0f, 1.0f});
}

Mat4 WorldCamera::clipTransform(float horizontalFov, float frameWidth, float frameHeight,
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

Mat4 CameraFrame::face(const Mat4& placement, std::uint32_t mode) const {
    if (mode == 0) {
        return placement;
    }
    Mat4 faced = placement;
    const Vec3 at{placement[3]};
    if (mode == kFacingFull) {
        // A proper rotation whose z points back at the camera.
        faced[0] = Vec4{-right, 0.0f};
        faced[1] = Vec4{up, 0.0f};
        faced[2] = Vec4{-forward, 0.0f};
        return faced;
    }
    const Vec3 toCamera = position - at;
    if (toCamera.x * toCamera.x + toCamera.z * toCamera.z < 1e-8f) {
        return placement;
    }
    const float yaw = std::atan2(toCamera.x, toCamera.z);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    faced[0] = Vec4{c, 0.0f, -s, 0.0f};
    faced[1] = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
    faced[2] = Vec4{s, 0.0f, c, 0.0f};
    return faced;
}

} // namespace gdl
