#pragma once

#include <algorithm>
#include <cstdint>
#include <numbers>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace gdl {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

constexpr float degreesToRadians(float degrees) {
    return degrees * (kPi / 180.0f);
}
constexpr float radiansToDegrees(float radians) {
    return radians * (180.0f / kPi);
}

/** Axis-aligned rectangle. */
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    constexpr float right() const { return x + width; }
    constexpr float bottom() const { return y + height; }

    bool operator==(const Rect&) const = default;
};

/** RGBA8 colour, stored in memory as R, G, B, A. */
struct Color {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;

    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                std::uint8_t a = 255) {
        return Color{r, g, b, a};
    }

    /** Builds a colour from 0..1 components, clamping out-of-range values. */
    static constexpr Color fromFloats(float r, float g, float b, float a = 1.0f) {
        return Color{toByte(r), toByte(g), toByte(b), toByte(a)};
    }

    static constexpr Color white() { return Color{255, 255, 255, 255}; }
    static constexpr Color black() { return Color{0, 0, 0, 255}; }
    static constexpr Color transparent() { return Color{0, 0, 0, 0}; }

    constexpr Color withAlpha(std::uint8_t alpha) const { return Color{r, g, b, alpha}; }

    Vec4 toVec4() const {
        return Vec4{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                    static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f};
    }

    bool operator==(const Color&) const = default;

private:
    static constexpr std::uint8_t toByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
    }
};

static_assert(sizeof(Color) == 4);

/** Scale and offset that fit a frame into a target surface while preserving aspect ratio. */
struct Letterbox {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    bool operator==(const Letterbox&) const = default;
};

constexpr Letterbox fitFrame(float frameWidth, float frameHeight, float targetWidth,
                             float targetHeight) {
    const float scale = std::min(targetWidth / frameWidth, targetHeight / frameHeight);
    return Letterbox{scale, 0.5f * (targetWidth - frameWidth * scale),
                     0.5f * (targetHeight - frameHeight * scale)};
}

/**
 * Orthographic projection for a top-left-origin pixel space.
 * Depth passes through unchanged (0..1, larger is nearer).
 */
inline Mat4 makeScreenProjection(float width, float height) {
    const float right = width;
    const float top = height;
    return glm::orthoLH_ZO(0.0f, right, 0.0f, top, 0.0f, 1.0f);
}

/** Projection that draws a frameWidth×frameHeight frame letterboxed into a target surface. */
inline Mat4 makeLetterboxProjection(float frameWidth, float frameHeight, float targetWidth,
                                    float targetHeight) {
    const Letterbox fit = fitFrame(frameWidth, frameHeight, targetWidth, targetHeight);
    Mat4 toTarget = glm::translate(Mat4{1.0f}, Vec3{fit.offsetX, fit.offsetY, 0.0f});
    toTarget = glm::scale(toTarget, Vec3{fit.scale, fit.scale, 1.0f});
    return makeScreenProjection(targetWidth, targetHeight) * toTarget;
}

} // namespace gdl
