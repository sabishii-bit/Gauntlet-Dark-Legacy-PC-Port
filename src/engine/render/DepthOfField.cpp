#include "engine/render/DepthOfField.h"

#include <algorithm>
#include <cmath>

namespace gdl {

f32 DepthOfField::viewDistance(const Vec2& uv, f32 depth) const {
    const Vec4 position = clipToView * Vec4{uv * 2.0f - Vec2{1.0f}, depth, 1.0f};
    return position.z / std::max(std::abs(position.w), 0.000001f);
}

f32 DepthOfField::blurFraction(f32 distance) const {
    const f32 fraction =
        std::clamp((distance - focusEnd) / std::max(transition, 0.001f), 0.0f, 1.0f);
    return fraction * fraction * (3.0f - 2.0f * fraction);
}

} // namespace gdl
