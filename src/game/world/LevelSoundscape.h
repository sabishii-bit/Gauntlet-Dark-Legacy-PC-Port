#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/assets/WorldData.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "game/world/AmbientSounds.h"
#include "game/world/LevelTriggers.h"

namespace gdl::game {

/** Owns a level's sound banks, playback, music, scroll voice, ambient loops and opening cues.
 * The output is borrowed and may be null. Close before destroying it. */
class LevelSoundscape {
public:
    LevelSoundscape() = default;
    LevelSoundscape(const LevelSoundscape&) = delete;
    LevelSoundscape& operator=(const LevelSoundscape&) = delete;
    LevelSoundscape(LevelSoundscape&&) = delete;
    LevelSoundscape& operator=(LevelSoundscape&&) = delete;
    ~LevelSoundscape() = default;

    void open(const std::filesystem::path& root, SoundPlayer* output, const LevelAudioInfo* info,
              char realm = 'L', bool boss = false);
    void bindAmbience(const WorldLayout& layout);
    void updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear, f32 volume);
    void startMusic(const AssetLocator* assets, f32 volume);
    /** Stop scene cues early in teardown, leaving ambient loops until close(). */
    void stopCues();
    /** Silence a suspended stage without releasing banks borrowed by its actors. */
    void suspend();
    /** Stops every voice started here before releasing its borrowed clips. */
    void close();

    /** Search level, common, then ambient banks; a broken first match stays silent. */
    SoundHandle playNamed(std::string_view name, f32 volume = 1.0f);
    SoundHandle playFrom(SoundSet& bank, std::string_view name);
    SoundHandle playPromotion(std::string_view name, SoundHandle after = kNoSound);
    enum class Narrator : u8 { Primary, Either };
    SoundHandle narrate(std::string_view name, Narrator which = Narrator::Either,
                        SoundHandle after = kNoSound);

    /** The narrator's queue (sndFxQueAddEx's announcer queue): lines play one after another,
     * at most sixteen waiting. An announcement asks for room first, giving how long it is
     * willing to wait behind what is already queued (never refused when negative); once let in,
     * its parts are queued in turn. */
    static constexpr usize kMostNarration = 16;
    static constexpr f32 kAlwaysRoom = -1.0f;
    bool narrationRoom(f32 maxWait) const;
    /** Queues a narrator line (with `Either`, one the level's own banks keep as well), or a
     * name clip of a character's own bank; kNoSound when the sound is missing or the queue is
     * full. */
    SoundHandle queueNarration(std::string_view name, Narrator which = Narrator::Either);
    SoundHandle queueNarrationFrom(SoundSet& bank, std::string_view name);
    static constexpr std::string_view kPojoName = "S_POJO2"; ///< Pojo's name before a line
    /** An announcement by name (AudioWithName): the character's name clip from its own bank,
     * or Pojo's when it carries him, then `lines`, queued together; nothing, and false, when
     * the queue has no room within `maxWait`. */
    bool announce(SoundSet& characterBank, std::string_view name, bool pojo,
                  std::span<const std::string_view> lines, f32 maxWait);
    /** While held nothing more is let into the queue: the good wizard has the floor. */
    void holdNarration(bool held) { m_narrationHeld = held; }
    /** Advances the queue's clock, letting finished lines go. */
    void updateNarration(f32 seconds);
    /** Seconds of queued narration still to come. */
    f64 narrationBacklog() const;
    void playPickup();
    void playFootstep(bool second);
    void speakOverScroll(std::string_view name);
    void stopVoice();
    void stop(SoundHandle handle);
    void opening(const TriggerOpening& event);
    void settled(const TriggerOpening& event);

    SoundHandle music() const { return m_music; }
    SoundHandle voice() const { return m_voice; }
    SoundHandle fieldSound() const {
        return m_openings.empty() ? kNoSound : m_openings.back().handle;
    }
    const AmbientSounds& ambience() const { return m_ambience; }

private:
    struct Opening {
        s32 target = -1;
        SoundHandle handle = kNoSound;
    };
    void playCommon(std::optional<u32> sound);
    SoundHandle track(SoundHandle handle);
    SoundHandle playOpening(s32 slot, bool settled);
    SoundHandle queue(SoundSet& bank, u32 sound);
    void clearNarration();

    SoundPlayer* m_output = nullptr;
    SoundSet m_common;
    SoundSet m_level;
    SoundSet m_ambient;
    SoundSet m_narrator;
    SoundSet m_narratorSecond;
    SoundSet m_promotions;
    AmbientSounds m_ambience; ///< cleared before its borrowed banks
    std::array<std::optional<u32>, 2> m_steps{};
    std::optional<u32> m_pickup;
    std::string m_stream;
    s32 m_streamParts = 1;
    char m_realm = 'L';
    bool m_boss = false;
    SoundHandle m_music = kNoSound;
    SoundHandle m_voice = kNoSound;
    std::vector<Opening> m_openings;
    std::vector<SoundHandle> m_voices; ///< includes queued narration; pruned as new sounds start
    f64 m_narrationClock = 0.0;
    std::vector<f64> m_narrationEnds; ///< when each queued line not yet over will end
    SoundHandle m_narrationTail = kNoSound;
    bool m_narrationHeld = false;
};

} // namespace gdl::game
