#pragma once

#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Decoded audio ready for the mixer: interleaved samples in [-1, 1]. */
struct SoundClip {
    u32 sampleRate = 0;
    u32 channels = 0;
    std::vector<f32> samples;

    usize frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    f64 seconds() const { return sampleRate == 0 ? 0.0 : static_cast<f64>(frames()) / sampleRate; }
};

/** One step of a sequenced sound: a clip, and how the sequence continues after it. */
struct SoundSequenceStep {
    const SoundClip* clip = nullptr;
    bool loopStart = false; ///< a later step can jump back here
    bool loopBack = false;  ///< after this clip, continue from the nearest earlier loop start
};

/** A sound as the game triggers it: clips played back to back, possibly forever. */
struct SoundSequence {
    std::vector<SoundSequenceStep> steps;
    f32 volume = 1.0f;

    bool loops() const;
};

} // namespace gdl
