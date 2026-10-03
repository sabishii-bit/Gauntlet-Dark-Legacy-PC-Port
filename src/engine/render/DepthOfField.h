#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** Optional far-field blur. Distances are camera-space units, not depth-buffer values.
 * Foreground objects stay sharp; the caller includes every party member in focusEnd.
 * clipToView must include the actual viewport mapping and reversed-Z projection.
 * This screen-space prototype uses depth-writing geometry: additive effects borrow
 * the depth behind them, rather than claiming a separate translucent depth layer. */
struct DepthOfField {
    Mat4 clipToView{1.0f};
    f32 focusEnd = 20.0f;
    f32 transition = 30.0f;
    f32 radiusAt1080 = 4.0f;

    f32 viewDistance(const Vec2& uv, f32 depth) const;
    f32 blurFraction(f32 distance) const;
};

} // namespace gdl
