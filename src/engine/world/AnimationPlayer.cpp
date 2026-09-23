#include "engine/world/AnimationPlayer.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl {

void AnimationPlayer::start(const TreeSequenceInfo& sequence, u32 index, f32 transitionSeconds,
                            f32 frame) {
    m_sequence = &sequence;
    m_index = index;
    const f32 rate = sequence.frameRate > 0 ? static_cast<f32>(sequence.frameRate) : kDefaultRate;
    m_secondsPerFrame = rate * kRateUnit / std::max(m_speed, 1e-6f);
    m_frame = frame > static_cast<f32>(sequence.frames) ? 0.0f : frame;
    m_time = m_frame * m_secondsPerFrame;
    m_transitionLength = std::max(transitionSeconds, 0.0f);
    m_transitionTime = 0.0f;
    m_finished = false;
    m_held = false;
}

void AnimationPlayer::stop() {
    m_sequence = nullptr;
    m_index = 0;
    m_time = 0.0f;
    m_frame = 0.0f;
    m_transitionLength = 0.0f;
    m_transitionTime = 0.0f;
    m_finished = false;
    m_held = false;
}

f32 AnimationPlayer::transition() const {
    if (m_transitionLength <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(m_transitionTime / m_transitionLength, 0.0f, 1.0f);
}

bool AnimationPlayer::advance(f32 seconds, bool repeat) {
    if (m_sequence == nullptr || m_sequence->frames <= 0) {
        m_finished = true;
        return false;
    }
    if (m_held) {
        return false;
    }
    // The first frame holds while the transition blends in; the clock starts from it after.
    if (transitioning()) {
        m_transitionTime += seconds;
        if (!transitioning()) {
            m_transitionTime = m_transitionLength;
            m_time = m_frame * m_secondsPerFrame;
        }
        return false;
    }
    m_finished = false;
    m_time += seconds;
    f32 t = m_time / m_secondsPerFrame;
    const f32 whole = std::floor(0.5f + t);
    if (!m_smooth || std::abs(t - whole) < kSnapWindow || m_secondsPerFrame < kTick) {
        t = whole;
    }
    const auto last = static_cast<f32>(m_sequence->frames - 1);
    if (t >= last + 0.5f) {
        m_finished = true;
        if (repeat) {
            m_time = 0.0f;
            m_frame = 0.0f;
        } else {
            m_frame = last;
            m_held = true;
        }
        return true;
    }
    m_frame = std::max(t, 0.0f);
    return false;
}

} // namespace gdl
