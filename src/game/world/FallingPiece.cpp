#include "game/world/FallingPiece.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr f32 kLeafGravityStep = 1.0f;
constexpr f32 kLeafSpin = 0.1745f;    ///< ten degrees a second
constexpr f32 kSinkSpin = 0.01745f;   ///< one degree a second
constexpr f32 kStepSlack = 0.000001f; ///< a frame due within this much is taken
constexpr std::array<f32, 8> kSpinDirections{-4, -3, -2, -1, 1, 2, 3, 4};
} // namespace

FallingProfile FallingProfile::of(s32 subtype) {
    if (subtype == kLeafFall) {
        return FallingProfile{kLeafGravityStep, kLeafSpin};
    }
    if (subtype == kRockSink) {
        return FallingProfile{FallingProfile{}.gravityStep, kSinkSpin};
    }
    return FallingProfile{};
}

f32 FallingPiece::bottomOf(const WorldLayout& layout) {
    return layout.minBounds().y - kFloorSentinelMargin - kDiscardDepth;
}

s32 FallingPiece::framesDue(f32& remainder, f32 seconds) {
    remainder += seconds;
    s32 frames = 0;
    while (remainder + kStepSlack >= kStep) {
        remainder -= kStep;
        ++frames;
    }
    return frames;
}

f32 FallingPiece::presentationFraction(f32 remainder, f32 updateSeconds, f32 renderAlpha) {
    if (renderAlpha < 0.0f) {
        return 1.0f;
    }
    return std::clamp((remainder + std::clamp(renderAlpha, 0.0f, 1.0f) * updateSeconds) / kStep,
                      0.0f, 1.0f);
}

Vec3 FallingPiece::presentedPosition(f32 alpha) const {
    return glm::mix(m_previousPosition, position, std::clamp(alpha, 0.0f, 1.0f));
}

Vec3 FallingPiece::presentedRotation(f32 alpha) const {
    return glm::mix(m_previousRotation, rotation, std::clamp(alpha, 0.0f, 1.0f));
}

void FallingPiece::place(const Vec3& where, const Vec3& angles, usize index) {
    position = where;
    rotation = angles;
    m_previousPosition = position;
    m_previousRotation = rotation;
    velocity = Vec3{0.0f};
    instance = index;
    visible = true;
}

void FallingPiece::advance(const FallingProfile& profile, f32 bottom) {
    if (!visible) {
        return;
    }
    m_previousPosition = position;
    m_previousRotation = rotation;
    velocity.y -= profile.gravityStep;
    rotation.x += profile.spin * kSpinDirections[instance & 7U] * kStep;
    rotation.z += profile.spin * kSpinDirections[(~instance) & 7U] * kStep;
    position += velocity * kStep;
    visible = position.y >= bottom;
}
} // namespace gdl::game
