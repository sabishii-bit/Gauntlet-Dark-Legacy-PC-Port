#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace gdl {

struct AudioStreamDesc {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 1;
};

/**
 * A queue of PCM frames pushed from the game thread and pulled by the mixer thread, resampled
 * to the output rate along a curve through four neighbouring frames and spread to stereo on
 * the way out. Its gains (volume, pan, a stop) slide to their new value over a few
 * milliseconds rather than stepping, so no change of them clicks.
 */
class AudioStream {
public:
    /** How long a gain change takes to complete, and a stopped stream to fade away. */
    static constexpr float kGainRamp = 0.005f;

    AudioStream(AudioStreamDesc desc, std::uint32_t outputRate);

    const AudioStreamDesc& desc() const { return m_desc; }

    /** Queues interleaved frames of desc().channels samples each. */
    void push(std::span<const float> interleaved);

    /** Marks the end of the data; the stream is drained once the queue empties. */
    void finish();

    /** Fades out over kGainRamp and drops whatever is queued past that, for cutting playback
     * short. */
    void stop();
    bool finished() const;
    bool drained() const;

    double queuedSeconds() const;
    void setVolume(float volume);
    float volume() const;
    /** Where the sound sits between the speakers: -1 fully left, 0 centred, 1 fully right,
     * at constant power. */
    void setPan(float pan);
    float pan() const;

    /** Adds this stream's contribution to an interleaved stereo buffer at the output rate. */
    void mixInto(std::span<float> stereoOut);

private:
    std::size_t queuedFramesLocked() const {
        return m_queue.size() / m_desc.channels - m_readFrame;
    }
    float at(std::size_t frame, std::size_t channel) const {
        return m_queue[frame * m_desc.channels + channel];
    }
    void compactLocked();

    mutable std::mutex m_mutex;
    AudioStreamDesc m_desc;
    double m_step;
    float m_gainStep; ///< how far a gain slides per output frame
    std::vector<float> m_queue;
    std::size_t m_readFrame = 0; ///< the frame being read; the one before it is kept for the curve
    double m_fraction = 0.0;
    float m_volume = 1.0f;
    float m_leftGain = 1.0f;
    float m_rightGain = 1.0f;
    float m_pan = 0.0f;
    float m_leftLevel = -1.0f; ///< the gains as they stand; negative before the first frame
    float m_rightLevel = -1.0f;
    bool m_finished = false;
    bool m_stopping = false;
};

} // namespace gdl
