#include "engine/world/AmbientDimmer.h"

#include <algorithm>
#include <cmath>

namespace gdl {

void AmbientDimmer::ask(float amount) {
    m_wanted = amount;
    m_holdLeft = kHoldSeconds;
}

void AmbientDimmer::reset() {
    m_wanted = 0.0f;
    m_current = 0.0f;
    m_holdLeft = 0.0f;
    m_frames = 0.0f;
}

void AmbientDimmer::update(float seconds) {
    m_holdLeft = std::max(m_holdLeft - seconds, 0.0f);
    m_frames += seconds / kFrameSeconds;
    while (m_frames >= 1.0f) {
        m_frames -= 1.0f;
        // Unasked for, what was wanted fades.
        if (m_holdLeft <= 0.0f && m_wanted != 0.0f) {
            m_wanted *= kDecay;
            if (std::abs(m_wanted) < kBrightenStep) {
                m_wanted = 0.0f;
            }
        }
        const float gap = m_wanted - m_current;
        m_current += std::clamp(gap, -kDarkenStep, kBrightenStep);
    }
}

float AmbientDimmer::applied(float ambient) const {
    return std::clamp(ambient + m_current, 0.0f, 1.0f);
}

} // namespace gdl
