#include "engine/world/AnimationPlayer.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl {
namespace {
constexpr f32 kTimeSlack = 0.000001f;
}

void AnimationPlayer::start(const TreeSequenceInfo& sequence, u32 index, f32 transitionSeconds,
                            f32 frame, f32 transitionElapsedSeconds) {
    ++m_generation;
    m_sequence = &sequence;
    m_index = index;
    const f32 rate = sequence.frameRate > 0 ? static_cast<f32>(sequence.frameRate) : kDefaultRate;
    m_secondsPerFrame = rate * kRateUnit / std::max(m_speed, 1e-6f);
    m_frame = frame > static_cast<f32>(sequence.frames) ? 0.0f : frame;
    m_time = m_frame * m_secondsPerFrame;
    m_transitionLength = std::max(transitionSeconds, 0.0f);
    m_transitionTime = std::clamp(transitionElapsedSeconds, 0.0f, m_transitionLength);
    m_finished = false;
    m_held = false;
}

void AnimationPlayer::stop() {
    ++m_generation;
    m_sequence = nullptr;
    m_index = 0;
    m_time = 0.0f;
    m_frame = 0.0f;
    m_transitionLength = 0.0f;
    m_transitionTime = 0.0f;
    m_finished = false;
    m_held = false;
}

f32 AnimationPlayer::presentationFrame() const {
    if (m_sequence == nullptr || m_sequence->frames <= 0) {
        return 0.0f;
    }
    if (m_held) {
        return m_frame;
    }
    return std::clamp(m_time / m_secondsPerFrame, 0.0f, static_cast<f32>(m_sequence->frames - 1));
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
    if (seconds <= 0.0f) {
        return false;
    }
    // The first frame holds while the transition blends in; the clock starts from it after.
    if (transitioning()) {
        const f32 consumed = std::min(seconds, m_transitionLength - m_transitionTime);
        m_transitionTime += consumed;
        seconds -= consumed;
        if (m_transitionTime + kTimeSlack >= m_transitionLength) {
            m_transitionTime = m_transitionLength;
            m_time = m_frame * m_secondsPerFrame;
        }
        if (transitioning() || seconds <= kTimeSlack) {
            return false;
        }
    }
    m_finished = false;
    m_time += seconds;
    if (repeat) {
        // Each authored frame owns one complete frame interval, including the last.
        // Rounding the displayed pose must not shorten that interval or discard time.
        const f32 duration = static_cast<f32>(m_sequence->frames) * m_secondsPerFrame;
        if (m_time + kTimeSlack >= duration) {
            ++m_generation;
            m_time = m_time < duration ? 0.0f : std::fmod(m_time, duration);
            m_finished = true;
        }
    }
    f32 t = m_time / m_secondsPerFrame;
    const f32 whole = std::floor(0.5f + t);
    if (!m_smooth || std::abs(t - whole) < kSnapWindow || m_secondsPerFrame < kTick) {
        t = whole;
    }
    const auto last = static_cast<f32>(m_sequence->frames - 1);
    if (!repeat && t >= last + 0.5f) {
        m_finished = true;
        m_frame = last;
        m_held = true;
        return true;
    }
    m_frame = std::clamp(t, 0.0f, last);
    return m_finished;
}

} // namespace gdl
