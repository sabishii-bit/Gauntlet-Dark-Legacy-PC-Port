#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/AudioStream.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/StreamSource.h"

namespace gdl {

using SoundHandle = std::uint32_t;
inline constexpr SoundHandle kNoSound = 0;

/** Mixing groups with their own volume setting. */
enum class SoundCategory : std::uint8_t { Effects, Music, Count };

/**
 * Plays sound sequences through the mixer, feeding each voice's stream clip by clip so that
 * looping music keeps going and one-shot effects end on their own, and plays stream sources
 * (music decoded from disk as it goes) the same way.
 */
class SoundPlayer {
public:
    static constexpr double kLookaheadSeconds = 1.5;

    explicit SoundPlayer(AudioMixer& mixer);

    /** Starts a sequence; returns kNoSound when it has nothing to play. */
    SoundHandle play(const SoundSequence& sequence, float volume = 1.0f,
                     SoundCategory category = SoundCategory::Effects);

    /** Starts `sequence` once `previous` has ended (at once when it is not playing) and
     * returns the handle it plays under, which reports playing while it waits. */
    SoundHandle playAfter(SoundHandle previous, const SoundSequence& sequence, float volume = 1.0f,
                          SoundCategory category = SoundCategory::Effects);
    /** Plays a source, decoding it a piece ahead of the mixer; a looping one starts over
     * whenever it runs out. Null or an unplayable source gives kNoSound. */
    SoundHandle playStream(std::shared_ptr<StreamSource> source, bool loop, float volume = 1.0f,
                           SoundCategory category = SoundCategory::Music);

    /** Volumes in [0, 1]; a voice plays at master x category x its own volume. */
    void setMasterVolume(float volume);
    void setCategoryVolume(SoundCategory category, float volume);
    float masterVolume() const { return m_masterVolume; }
    float categoryVolume(SoundCategory category) const;

    /** Changes a playing voice's own volume (0..1) or pan (-1..1); unknown handles are
     * ignored. */
    void setVolume(SoundHandle handle, float volume);
    void setPan(SoundHandle handle, float pan);
    /** Fades a voice (or every voice) out over a few milliseconds; it no longer plays. */
    void stop(SoundHandle handle);
    void stopAll();
    bool isPlaying(SoundHandle handle) const;
    std::size_t voiceCount() const { return m_voices.size(); }

    /** Feeds voices that are running low and drops finished ones; call once per frame. */
    void update();

private:
    struct Voice {
        SoundHandle handle = kNoSound;
        std::shared_ptr<AudioStream> stream;
        std::shared_ptr<StreamSource> source; ///< set for a stream voice
        bool loop = false;
        SoundSequence sequence;
        SoundCategory category = SoundCategory::Effects;
        float volume = 1.0f;
        std::size_t nextStep = 0;
        bool finished = false; ///< fed to the end
        bool stopped = false;  ///< cut short: fading out, no longer playing
    };

    /** A sequence waiting for another voice to end. */
    struct Pending {
        SoundHandle handle = kNoSound;
        SoundHandle after = kNoSound;
        SoundSequence sequence;
        float volume = 1.0f;
        SoundCategory category = SoundCategory::Effects;
    };

    SoundHandle start(const SoundSequence& sequence, float volume, SoundCategory category,
                      SoundHandle handle);

    void applyVolume(Voice& voice) const;

    void feed(Voice& voice);
    void feedSource(Voice& voice);
    static void pushClip(AudioStream& stream, const SoundClip& clip);

    AudioMixer& m_mixer;
    std::vector<Voice> m_voices;
    std::vector<Pending> m_pending;
    std::vector<float> m_scratch; ///< frames read from a source on their way to its stream
    SoundHandle m_nextHandle = 1;
    float m_masterVolume = 1.0f;
    std::array<float, static_cast<std::size_t>(SoundCategory::Count)> m_categoryVolumes{1.0f, 1.0f};
};

} // namespace gdl
