#include "engine/audio/AudioMixer.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl {

AudioMixer::AudioMixer(u32 outputRate)
    : m_outputRate(outputRate), m_releaseStep(1.0f / (kRelease * static_cast<f32>(outputRate))) {}

std::shared_ptr<AudioStream> AudioMixer::createStream(const AudioStreamDesc& desc) {
    auto stream = std::make_shared<AudioStream>(desc, m_outputRate);
    const std::scoped_lock lock(m_mutex);
    m_streams.push_back(stream);
    return stream;
}

void AudioMixer::mix(std::span<f32> stereoOut) {
    std::ranges::fill(stereoOut, 0.0f);
    const std::scoped_lock lock(m_mutex);
    if (m_paused) {
        return;
    }
    for (const auto& stream : m_streams) {
        stream->mixInto(stereoOut);
    }
    std::erase_if(m_streams, [](const std::shared_ptr<AudioStream>& stream) {
        const bool abandoned = stream.use_count() == 1 && stream->queuedSeconds() == 0.0;
        return stream->drained() || abandoned;
    });
    limit(stereoOut);
}

void AudioMixer::setPaused(bool paused) {
    const std::scoped_lock lock(m_mutex);
    m_paused = paused;
}

void AudioMixer::limit(std::span<f32> stereoOut) {
    for (usize frame = 0; frame + 1 < stereoOut.size(); frame += 2) {
        const f32 peak = std::max(std::abs(stereoOut[frame]), std::abs(stereoOut[frame + 1]));
        const f32 needed = peak > kCeiling ? kCeiling / peak : 1.0f;
        m_limiterGain =
            needed < m_limiterGain ? needed : std::min(m_limiterGain + m_releaseStep, 1.0f);
        stereoOut[frame] = std::clamp(stereoOut[frame] * m_limiterGain, -kCeiling, kCeiling);
        stereoOut[frame + 1] =
            std::clamp(stereoOut[frame + 1] * m_limiterGain, -kCeiling, kCeiling);
    }
}

usize AudioMixer::streamCount() const {
    const std::scoped_lock lock(m_mutex);
    return m_streams.size();
}

} // namespace gdl
