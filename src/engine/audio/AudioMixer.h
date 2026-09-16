#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "engine/audio/AudioStream.h"
#include "engine/core/Types.h"

namespace gdl {

/** Sums every live AudioStream into interleaved stereo output. Safe to call from any thread. */
class AudioMixer {
public:
    static constexpr u32 kOutputChannels = 2;

    explicit AudioMixer(u32 outputRate);

    u32 outputRate() const { return m_outputRate; }

    std::shared_ptr<AudioStream> createStream(const AudioStreamDesc& desc);

    /** Overwrites `stereoOut` with the mix of all streams and drops the ones that finished. */
    void mix(std::span<f32> stereoOut);

    usize streamCount() const;

private:
    u32 m_outputRate;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<AudioStream>> m_streams;
};

} // namespace gdl
