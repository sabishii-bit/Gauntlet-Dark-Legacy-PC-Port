#include "engine/render/HeatDistortion.h"

#include <algorithm>
#include <cmath>

namespace gdl {
HeatSource HeatSource::project(const Mat4& clip, const Vec3& center, const Vec3& right,
                               const Vec3& up, f32 radius, f32 seconds) {
    const Vec4 point = clip * Vec4{center, 1.0f};
    if (!(point.w > 0.001f) || !(radius > 0.0f)) {
        return {};
    }
    const Vec4 x = clip * Vec4{center + right * radius, 1.0f};
    const Vec4 y = clip * Vec4{center + up * radius, 1.0f};
    if (!std::isfinite(x.w) || !std::isfinite(y.w) || x.w <= 0.001f || y.w <= 0.001f) {
        return {};
    }
    const Vec2 uv = Vec2{point} / point.w * 0.5f + Vec2{0.5f};
    const Vec2 extent{std::abs(x.x / x.w - point.x / point.w) * 0.5f,
                      std::abs(y.y / y.w - point.y / point.w) * 0.5f};
    return {uv, extent, point.z / point.w, seconds};
}

void HeatDistortion::add(const HeatSource& source) {
    if (!std::isfinite(source.center.x) || !std::isfinite(source.center.y) ||
        !std::isfinite(source.radius.x) || !std::isfinite(source.radius.y) ||
        !std::isfinite(source.seconds) || !std::isfinite(source.depth) || source.depth <= 0.0f ||
        source.depth > 1.0f || source.radius.x <= 0.0f || source.radius.y <= 0.0f ||
        source.center.x + source.radius.x < 0.0f || source.center.x - source.radius.x > 1.0f ||
        source.center.y + source.radius.y < 0.0f || source.center.y - source.radius.y > 1.0f) {
        return;
    }
    HeatSource candidate = source;
    // Never turn a near-plane fire into a full-screen wobble.
    candidate.radius = glm::min(candidate.radius, Vec2{0.2f});
    for (auto& slot : m_sources) {
        if (candidate.radius.x * candidate.radius.y > slot.radius.x * slot.radius.y) {
            std::swap(candidate, slot);
        }
    }
}
} // namespace gdl
