#include "engine/audio/AudioMixer.h"

#include <algorithm>

namespace gdl {

AudioMixer::AudioMixer(u32 outputRate) : m_outputRate(outputRate) {}

std::shared_ptr<AudioStream> AudioMixer::createStream(const AudioStreamDesc& desc) {
    auto stream = std::make_shared<AudioStream>(desc, m_outputRate);
    const std::scoped_lock lock(m_mutex);
    m_streams.push_back(stream);
    return stream;
}

void AudioMixer::mix(std::span<f32> stereoOut) {
    std::ranges::fill(stereoOut, 0.0f);
    const std::scoped_lock lock(m_mutex);
    for (const auto& stream : m_streams) {
        stream->mixInto(stereoOut);
    }
    std::erase_if(m_streams, [](const std::shared_ptr<AudioStream>& stream) {
        const bool abandoned = stream.use_count() == 1 && stream->queuedSeconds() == 0.0;
        return stream->drained() || abandoned;
    });
}

usize AudioMixer::streamCount() const {
    const std::scoped_lock lock(m_mutex);
    return m_streams.size();
}

} // namespace gdl
