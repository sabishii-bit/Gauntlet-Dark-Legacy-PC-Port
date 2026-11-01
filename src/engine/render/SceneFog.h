#pragma once

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** Perspective fog evaluated from camera-space depth, independently of the
 * renderer's reversed-Z projection. Zero coefficients leave the scene unchanged. */
struct SceneFog {
    Vec3 color{1.0f};
    f32 reciprocalScale = 0;
    f32 bias = 0;

    f32 atDepth(f32 depth) const {
        return depth > 0 ? std::clamp(bias + reciprocalScale / depth, 0.0f, 1.0f) : 0;
    }
    bool valid() const {
        return std::isfinite(reciprocalScale) && std::isfinite(bias) && std::isfinite(color.x) &&
               std::isfinite(color.y) && std::isfinite(color.z);
    }
    bool operator==(const SceneFog&) const = default;
    template <class Visitor> friend void visitNativeState(Visitor& visitor, const SceneFog& value) {
        visitor.template fields<3>(value);
    }
};

} // namespace gdl
