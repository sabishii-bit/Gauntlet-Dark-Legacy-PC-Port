#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gdl {

/** Decoded audio ready for the mixer: interleaved samples in [-1, 1]. */
struct SoundClip {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<float> samples;

    std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
    double seconds() const {
        return sampleRate == 0 ? 0.0 : static_cast<double>(frames()) / sampleRate;
    }
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
    float volume = 1.0f;

    bool loops() const;
};

} // namespace gdl
