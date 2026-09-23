#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "engine/audio/AudioStream.h"
#include "engine/core/Types.h"

namespace gdl {

/**
 * Sums every live AudioStream into interleaved stereo output and holds the sum under full
 * scale: a peak beyond it turns the whole mix down at once, and the mix comes back up over
 * the release. Safe to call from any thread.
 */
class AudioMixer {
public:
    static constexpr u32 kOutputChannels = 2;
    static constexpr f32 kCeiling = 1.0f;  ///< the loudest the mix is let reach
    static constexpr f32 kRelease = 0.05f; ///< seconds the mix takes to come back up

    explicit AudioMixer(u32 outputRate);

    u32 outputRate() const { return m_outputRate; }

    std::shared_ptr<AudioStream> createStream(const AudioStreamDesc& desc);

    /** Overwrites `stereoOut` with the mix of all streams and drops the ones that finished. */
    void mix(std::span<f32> stereoOut);

    usize streamCount() const;

private:
    /** Turns the mix down wherever it would pass the ceiling. */
    void limit(std::span<f32> stereoOut);

    u32 m_outputRate;
    f32 m_releaseStep; ///< how far the limiter's gain rises per frame
    f32 m_limiterGain = 1.0f;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<AudioStream>> m_streams;
};

} // namespace gdl
