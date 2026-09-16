#pragma once

#include <memory>
#include <vector>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/AudioStream.h"
#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

namespace gdl {

using SoundHandle = u32;
inline constexpr SoundHandle kNoSound = 0;

/**
 * Plays sound sequences through the mixer, feeding each voice's stream clip by clip so that
 * looping music keeps going and one-shot effects end on their own.
 */
class SoundPlayer {
public:
    static constexpr f64 kLookaheadSeconds = 1.5;

    explicit SoundPlayer(AudioMixer& mixer);

    /** Starts a sequence; returns kNoSound when it has nothing to play. */
    SoundHandle play(const SoundSequence& sequence, f32 volume = 1.0f);

    void stop(SoundHandle handle);
    void stopAll();
    bool isPlaying(SoundHandle handle) const;
    usize voiceCount() const { return m_voices.size(); }

    /** Feeds voices that are running low and drops finished ones; call once per frame. */
    void update();

private:
    struct Voice {
        SoundHandle handle = kNoSound;
        std::shared_ptr<AudioStream> stream;
        SoundSequence sequence;
        usize nextStep = 0;
        bool finished = false;
    };

    static void feed(Voice& voice);
    static void pushClip(AudioStream& stream, const SoundClip& clip);

    AudioMixer& m_mixer;
    std::vector<Voice> m_voices;
    SoundHandle m_nextHandle = 1;
};

} // namespace gdl
