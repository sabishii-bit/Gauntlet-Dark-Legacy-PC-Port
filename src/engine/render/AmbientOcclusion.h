#pragma once

#include "engine/math/Math.h"

namespace gdl {
/** Optional screen-space contact shading, not a replacement for authored lighting.
 * Only depth-writing surfaces participate; hidden/off-screen geometry is unavailable. */
struct AmbientOcclusion {
    Mat4 clipToView{1.0f};
    f32 radius = 2.0f;
    f32 bias = 0.08f;
    f32 strength = 0.25f; ///< maximum darkening, deliberately subtle on prelit environments
};
} // namespace gdl
