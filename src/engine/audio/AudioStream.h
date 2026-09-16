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
 * linearly to the output rate and spread to stereo on the way out.
 */
class AudioStream {
public:
    AudioStream(AudioStreamDesc desc, u32 outputRate);

    const AudioStreamDesc& desc() const { return m_desc; }

    /** Queues interleaved frames of desc().channels samples each. */
    void push(std::span<const f32> interleaved);

    /** Marks the end of the data; the stream is drained once the queue empties. */
    void finish();

    /** Drops everything still queued and finishes at once, for cutting playback short. */
    void stop();
    bool finished() const;
    bool drained() const;

    f64 queuedSeconds() const;
    void setVolume(f32 volume);
    f32 volume() const;

    /** Adds this stream's contribution to an interleaved stereo buffer at the output rate. */
    void mixInto(std::span<f32> stereoOut);

private:
    usize queuedFramesLocked() const { return m_queue.size() / m_desc.channels - m_readFrame; }
    void compactLocked();

    mutable std::mutex m_mutex;
    AudioStreamDesc m_desc;
    f64 m_step;
    std::vector<f32> m_queue;
    usize m_readFrame = 0;
    f64 m_fraction = 0.0;
    f32 m_volume = 1.0f;
    bool m_finished = false;
};

} // namespace gdl
