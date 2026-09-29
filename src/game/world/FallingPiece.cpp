#include "game/world/FallingPiece.h"

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

void FallingPiece::place(const Vec3& where, const Vec3& angles, usize index) {
    position = where;
    rotation = angles;
    velocity = Vec3{0.0f};
    instance = index;
    visible = true;
}

void FallingPiece::advance(const FallingProfile& profile, f32 bottom) {
    if (!visible) {
        return;
    }
    velocity.y -= profile.gravityStep;
    rotation.x += profile.spin * kSpinDirections[instance & 7U] * kStep;
    rotation.z += profile.spin * kSpinDirections[(~instance) & 7U] * kStep;
    position += velocity * kStep;
    visible = position.y >= bottom;
}
} // namespace gdl::game
