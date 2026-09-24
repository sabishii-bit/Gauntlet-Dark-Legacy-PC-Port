#pragma once

#include <array>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** The committed portal departure: held controls, lightning skin, then a sinking spin.
 * Presentation only; the logical player position stays at the portal for the camera. */
class PortalDeparture {
public:
    static constexpr s32 kTicks = 50;
    static constexpr f32 kSinkPerTick = 0.12f;
    static constexpr f32 kSpinPerSecond = 3.0f * glm::pi<f32>();
    void begin(RenderDevice& device, TextureSet& weapons);
    void clear();
    void update(s32 ticks);
    bool started() const { return m_started; }
    bool finished() const { return m_started && m_ticks >= kTicks; }
    Mat4 transform(const Mat4& body) const;
    const Texture* skin() const;

private:
    std::array<const Texture*, 10> m_frames{};
    s32 m_ticks = 0;
    bool m_started = false;
};

} // namespace gdl::game
