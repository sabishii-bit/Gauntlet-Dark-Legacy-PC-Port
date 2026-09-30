#include "game/app/MusicDuck.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {
constexpr f64 kLongestUpdate = 1.0; ///< a stalled frame steps no further than this
}

void MusicDuck::update(f64 seconds, bool ducked) {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return;
    }
    m_remainder += std::min(seconds, kLongestUpdate) / kFrameSeconds;
    const auto frames = static_cast<s32>(std::floor(m_remainder));
    m_remainder -= frames;
    const f32 target = ducked ? 0.0f : 1.0f;
    const f32 change = std::min(kStep * static_cast<f32>(frames), std::abs(target - m_level));
    m_level = std::clamp(m_level + (target < m_level ? -change : change), 0.0f, 1.0f);
}

} // namespace gdl::game
