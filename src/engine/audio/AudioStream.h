#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

struct AudioStreamDesc {
    u32 sampleRate = 48000;
    u32 channels = 1;
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
    static constexpr f32 kGainRamp = 0.005f;

    AudioStream(AudioStreamDesc desc, u32 outputRate);

    const AudioStreamDesc& desc() const { return m_desc; }

    /** Queues interleaved frames of desc().channels samples each. */
    void push(std::span<const f32> interleaved);

    /** Marks the end of the data; the stream is drained once the queue empties. */
    void finish();

    /** Fades out over kGainRamp and drops whatever is queued past that, for cutting playback
     * short. */
    void stop();
    bool finished() const;
    bool drained() const;

    f64 queuedSeconds() const;
    void setVolume(f32 volume);
    f32 volume() const;
    /** Where the sound sits between the speakers: -1 fully left, 0 centred, 1 fully right,
     * at constant power. */
    void setPan(f32 pan);
    f32 pan() const;

    /** Adds this stream's contribution to an interleaved stereo buffer at the output rate. */
    void mixInto(std::span<f32> stereoOut);

private:
    usize queuedFramesLocked() const { return m_queue.size() / m_desc.channels - m_readFrame; }
    f32 at(usize frame, usize channel) const { return m_queue[frame * m_desc.channels + channel]; }
    void compactLocked();

    mutable std::mutex m_mutex;
    AudioStreamDesc m_desc;
    f64 m_step;
    f32 m_gainStep; ///< how far a gain slides per output frame
    std::vector<f32> m_queue;
    usize m_readFrame = 0; ///< the frame being read; the one before it is kept for the curve
    f64 m_fraction = 0.0;
    f32 m_volume = 1.0f;
    f32 m_leftGain = 1.0f;
    f32 m_rightGain = 1.0f;
    f32 m_pan = 0.0f;
    f32 m_leftLevel = -1.0f; ///< the gains as they stand; negative before the first frame
    f32 m_rightLevel = -1.0f;
    bool m_finished = false;
    bool m_stopping = false;
};

} // namespace gdl
