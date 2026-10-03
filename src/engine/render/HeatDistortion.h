#pragma once

#include <array>

#include "engine/math/Math.h"

namespace gdl {
struct HeatSource {
    Vec2 center{0.0f}; ///< framebuffer UV, including camera letterboxing
    Vec2 radius{0.0f};
    f32 depth = 0.0f;   ///< reversed-Z depth of the source
    f32 seconds = 0.0f; ///< owner's interpolated simulation clock, held during pause

    static HeatSource project(const Mat4& clip, const Vec3& center, const Vec3& right,
                              const Vec3& up, f32 radius, f32 seconds);
};

/** A bounded presentation budget: retain the four largest visible emitters, never pixels
 * selected by brightness. Cleared every frame so dead sources cannot leave heat behind. */
class HeatDistortion {
public:
    static constexpr usize kCapacity = 4;
    void add(const HeatSource& source);
    const std::array<HeatSource, kCapacity>& sources() const { return m_sources; }

private:
    std::array<HeatSource, kCapacity> m_sources{};
};
} // namespace gdl
