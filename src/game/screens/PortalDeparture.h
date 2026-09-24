#pragma once

#include <array>
#include <string_view>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** The committed portal departure: held controls, lightning skin, then a sinking spin.
 * The camera follows the same downward displacement as the rendered bodies. */
class PortalDeparture {
public:
    static constexpr s32 kTicks = 50;
    static constexpr std::string_view kSound = "S_TUNNEL";
    static constexpr f32 kSinkPerTick = 0.12f;
    static constexpr f32 kSpinPerSecond = 3.0f * glm::pi<f32>();
    void begin(RenderDevice& device, TextureSet& weapons);
    void clear();
    void update(s32 ticks);
    bool started() const { return m_started; }
    bool finished() const { return m_started && m_ticks >= kTicks; }
    Vec3 displacement() const { return {0, -static_cast<f32>(m_ticks) * kSinkPerTick, 0}; }
    Mat4 transform(const Mat4& body) const;
    const Texture* skin() const;

private:
    std::array<const Texture*, 10> m_frames{};
    s32 m_ticks = 0;
    bool m_started = false;
};

} // namespace gdl::game
