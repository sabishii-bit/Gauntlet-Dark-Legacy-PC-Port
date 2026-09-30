#include "game/world/VoiceQueue.h"

#include <algorithm>

namespace gdl::game {

bool VoiceQueue::room(f32 maxWait) const {
    if (m_held) {
        return false;
    }
    const f64 waiting = backlog();
    if (waiting > 0.0 && m_ends.size() >= kMost) {
        return false;
    }
    return maxWait < 0.0f || waiting <= static_cast<f64>(maxWait);
}

f64 VoiceQueue::backlog() const {
    // Lines stopped from elsewhere no longer wait, whatever the clock says.
    if (m_ends.empty() || m_output == nullptr || !m_output->isPlaying(m_tail)) {
        return 0.0;
    }
    return std::max(m_ends.back() - m_clock, 0.0);
}

f64 VoiceQueue::lengthOf(const SoundSequence& sequence) {
    f64 length = 0.0;
    for (const SoundSequenceStep& step : sequence.steps) {
        length += step.clip != nullptr ? step.clip->seconds() : 0.0;
    }
    return length;
}

SoundHandle VoiceQueue::queue(const SoundSequence& sequence, f32 volume) {
    if (m_output == nullptr || m_held) {
        return kNoSound;
    }
    if (!m_output->isPlaying(m_tail)) {
        m_ends.clear();
    }
    if (m_ends.size() >= kMost) {
        return kNoSound;
    }
    const SoundHandle handle =
        m_output->playAfter(m_tail, sequence, volume, SoundCategory::Effects);
    if (handle == kNoSound) {
        return kNoSound;
    }
    const f64 start = m_clock + backlog();
    m_ends.push_back(start + lengthOf(sequence));
    m_tail = handle;
    return handle;
}

void VoiceQueue::update(f32 seconds) {
    m_clock += std::max(seconds, 0.0f);
    std::erase_if(m_ends, [this](f64 end) { return end <= m_clock; });
}

void VoiceQueue::clear() {
    m_ends.clear();
    m_tail = kNoSound;
    m_held = false;
}

} // namespace gdl::game
