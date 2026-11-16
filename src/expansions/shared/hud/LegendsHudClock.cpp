#include "expansions/shared/hud/LegendsHudClock.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

s64 LegendsHudClock::advance(f32 seconds) {
    m_rewound = false;
    if (!std::isfinite(seconds)) {
        return 0;
    }
    const auto target =
        static_cast<s64>(std::floor(static_cast<f64>(seconds) * f64{kStepsPerSecond}));
    if (!m_started || target < m_step) {
        m_rewound = m_started;
        m_started = true;
        m_step = target;
        return 0;
    }
    const s64 passed = target - m_step;
    m_step = target;
    return std::min(passed, kMostCatchUp);
}

} // namespace gdl::game
