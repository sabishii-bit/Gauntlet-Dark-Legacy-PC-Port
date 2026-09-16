#pragma once

#include <algorithm>
#include <numbers>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "engine/core/Types.h"

namespace gdl {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr f32 kPi = std::numbers::pi_v<f32>;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;

constexpr f32 degreesToRadians(f32 degrees) {
    return degrees * (kPi / 180.0f);
}
constexpr f32 radiansToDegrees(f32 radians) {
    return radians * (180.0f / kPi);
}

/** Axis-aligned rectangle. */
struct Rect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;

    constexpr f32 right() const { return x + width; }
    constexpr f32 bottom() const { return y + height; }

    bool operator==(const Rect&) const = default;
};

/** RGBA8 colour, stored in memory as R, G, B, A. */
struct Color {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;

    static constexpr Color rgba(u8 r, u8 g, u8 b, u8 a = 255) { return Color{r, g, b, a}; }

    /** Builds a colour from 0..1 components, clamping out-of-range values. */
    static constexpr Color fromFloats(f32 r, f32 g, f32 b, f32 a = 1.0f) {
        return Color{toByte(r), toByte(g), toByte(b), toByte(a)};
    }

    static constexpr Color white() { return Color{255, 255, 255, 255}; }
    static constexpr Color black() { return Color{0, 0, 0, 255}; }
    static constexpr Color transparent() { return Color{0, 0, 0, 0}; }

    constexpr Color withAlpha(u8 alpha) const { return Color{r, g, b, alpha}; }

    Vec4 toVec4() const {
        return Vec4{static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                    static_cast<f32>(b) / 255.0f, static_cast<f32>(a) / 255.0f};
    }

    bool operator==(const Color&) const = default;

private:
    static constexpr u8 toByte(f32 value) {
        return static_cast<u8>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
    }
};

static_assert(sizeof(Color) == 4);

/** Scale and offset that fit a frame into a target surface while preserving aspect ratio. */
struct Letterbox {
    f32 scale = 1.0f;
    f32 offsetX = 0.0f;
    f32 offsetY = 0.0f;

    bool operator==(const Letterbox&) const = default;
};

constexpr Letterbox fitFrame(f32 frameWidth, f32 frameHeight, f32 targetWidth, f32 targetHeight) {
    const f32 scale = std::min(targetWidth / frameWidth, targetHeight / frameHeight);
    return Letterbox{scale, 0.5f * (targetWidth - frameWidth * scale),
                     0.5f * (targetHeight - frameHeight * scale)};
}

/**
 * Orthographic projection for a top-left-origin pixel space.
 * Depth passes through unchanged (0..1, larger is nearer).
 */
inline Mat4 makeScreenProjection(f32 width, f32 height) {
    const f32 right = width;
    const f32 top = height;
    return glm::orthoLH_ZO(0.0f, right, 0.0f, top, 0.0f, 1.0f);
}

/** Projection that draws a frameWidth×frameHeight frame letterboxed into a target surface. */
inline Mat4 makeLetterboxProjection(f32 frameWidth, f32 frameHeight, f32 targetWidth,
                                    f32 targetHeight) {
    const Letterbox fit = fitFrame(frameWidth, frameHeight, targetWidth, targetHeight);
    Mat4 toTarget = glm::translate(Mat4{1.0f}, Vec3{fit.offsetX, fit.offsetY, 0.0f});
    toTarget = glm::scale(toTarget, Vec3{fit.scale, fit.scale, 1.0f});
    return makeScreenProjection(targetWidth, targetHeight) * toTarget;
}

} // namespace gdl
