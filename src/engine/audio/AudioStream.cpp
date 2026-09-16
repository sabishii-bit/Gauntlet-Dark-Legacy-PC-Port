#include "engine/audio/AudioStream.h"

#include <algorithm>

#include "engine/core/Assert.h"

namespace gdl {

AudioStream::AudioStream(AudioStreamDesc desc, u32 outputRate)
    : m_desc(desc), m_step(static_cast<f64>(desc.sampleRate) / outputRate) {
    GDL_VERIFY(desc.channels > 0 && desc.sampleRate > 0 && outputRate > 0,
               "AudioStream needs non-zero channels and sample rates");
}

void AudioStream::push(std::span<const f32> interleaved) {
    const std::scoped_lock lock(m_mutex);
    m_queue.insert(m_queue.end(), interleaved.begin(), interleaved.end());
}

void AudioStream::finish() {
    const std::scoped_lock lock(m_mutex);
    m_finished = true;
}

void AudioStream::stop() {
    const std::scoped_lock lock(m_mutex);
    m_queue.clear();
    m_readFrame = 0;
    m_fraction = 0.0;
    m_finished = true;
}

bool AudioStream::finished() const {
    const std::scoped_lock lock(m_mutex);
    return m_finished;
}

bool AudioStream::drained() const {
    const std::scoped_lock lock(m_mutex);
    return m_finished && queuedFramesLocked() == 0;
}

f64 AudioStream::queuedSeconds() const {
    const std::scoped_lock lock(m_mutex);
    return static_cast<f64>(queuedFramesLocked()) / m_desc.sampleRate;
}

void AudioStream::setVolume(f32 volume) {
    const std::scoped_lock lock(m_mutex);
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

f32 AudioStream::volume() const {
    const std::scoped_lock lock(m_mutex);
    return m_volume;
}

void AudioStream::mixInto(std::span<f32> stereoOut) {
    const std::scoped_lock lock(m_mutex);
    const usize channels = m_desc.channels;
    const usize outputFrames = stereoOut.size() / 2;

    for (usize frame = 0; frame < outputFrames; ++frame) {
        const usize available = queuedFramesLocked();
        if (available == 0) {
            break;
        }
        const usize current = m_readFrame * channels;
        const usize following = available >= 2 ? current + channels : current;
        const auto fraction = static_cast<f32>(m_fraction);
        const auto sample = [&](usize channel) {
            const f32 a = m_queue[current + channel];
            const f32 b = m_queue[following + channel];
            return (a + (b - a) * fraction) * m_volume;
        };
        const f32 left = sample(0);
        const f32 right = channels > 1 ? sample(1) : left;
        stereoOut[frame * 2] += left;
        stereoOut[frame * 2 + 1] += right;

        m_fraction += m_step;
        while (m_fraction >= 1.0) {
            m_fraction -= 1.0;
            ++m_readFrame;
        }
    }
    compactLocked();
}

void AudioStream::compactLocked() {
    const usize consumed = m_readFrame * m_desc.channels;
    if (consumed == 0 || consumed < m_queue.size() / 2) {
        return;
    }
    m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(consumed));
    m_readFrame = 0;
}

} // namespace gdl
