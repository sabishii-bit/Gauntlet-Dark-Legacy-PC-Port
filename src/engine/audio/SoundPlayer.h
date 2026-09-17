#pragma once

#include <array>
#include <memory>
#include <vector>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/AudioStream.h"
#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

namespace gdl {

using SoundHandle = u32;
inline constexpr SoundHandle kNoSound = 0;

/** Mixing groups with their own volume setting. */
enum class SoundCategory : u8 { Effects, Music, Count };

/**
 * Plays sound sequences through the mixer, feeding each voice's stream clip by clip so that
 * looping music keeps going and one-shot effects end on their own.
 */
class SoundPlayer {
public:
    static constexpr f64 kLookaheadSeconds = 1.5;

    explicit SoundPlayer(AudioMixer& mixer);

    /** Starts a sequence; returns kNoSound when it has nothing to play. */
    SoundHandle play(const SoundSequence& sequence, f32 volume = 1.0f,
                     SoundCategory category = SoundCategory::Effects);

    /** Volumes in [0, 1]; a voice plays at master x category x its own volume. */
    void setMasterVolume(f32 volume);
    void setCategoryVolume(SoundCategory category, f32 volume);
    f32 masterVolume() const { return m_masterVolume; }
    f32 categoryVolume(SoundCategory category) const;

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
        SoundCategory category = SoundCategory::Effects;
        f32 volume = 1.0f;
        usize nextStep = 0;
        bool finished = false;
    };

    void applyVolume(Voice& voice) const;

    static void feed(Voice& voice);
    static void pushClip(AudioStream& stream, const SoundClip& clip);

    AudioMixer& m_mixer;
    std::vector<Voice> m_voices;
    SoundHandle m_nextHandle = 1;
    f32 m_masterVolume = 1.0f;
    std::array<f32, static_cast<usize>(SoundCategory::Count)> m_categoryVolumes{1.0f, 1.0f};
};

} // namespace gdl
