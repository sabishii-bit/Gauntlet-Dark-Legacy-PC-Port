#include "game/players/BodyGlow.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

void BodyGlow::step(f32 seconds) {
    if (m_level == 0.0f || seconds <= 0.0f) {
        return;
    }
    m_level *= std::pow(kFade, seconds * kFrameRate);
    if (std::abs(m_level) < kGone) {
        m_level = 0.0f;
    }
}

WorldLighting BodyGlow::apply(const WorldLighting& lighting) const {
    WorldLighting lit = lighting;
    lit.ambient += Vec3{m_level};
    return lit;
}

} // namespace gdl::game
