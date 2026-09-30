#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/assets/WorldData.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/audio/StreamPlaylist.h"
#include "engine/audio/StreamSource.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "game/world/AmbientSounds.h"
#include "game/world/LevelTriggers.h"
#include "game/world/MusicAreas.h"

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
    /** Binds the level's ambient loops and its music zones. */
    void bindAmbience(const WorldLayout& layout);
    void updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear, f32 volume);
    /** Lets the zones ask for the area holding the party (items.c 4514-4522, 4733-4736). */
    void updateMusicAreas(std::span<const Vec3> listeners);
    /** Starts the first area's stream at `volume`, the level's; `assets` is borrowed for the
     * other areas' streams until the music stops. */
    void startMusic(const AssetLocator* assets, f32 volume);
    /** Asks for an area's stream and how to go over to it (sMusicSubIndex and sMusicSubState);
     * `updateMusic` carries it out. A level with one area ignores it. */
    void selectMusicArea(s32 area, MusicSwitch how);
    /** Told every frame whether the boss is awake: its waking asks for the second area with
     * a fade (BossActivate, boss.c 695). */
    void bossAwake(bool awake);
    /** The original's per-frame music service (AudioMusicVolUpdate, thirty a second of game
     * time): a fade of `kFadeStep` a frame down to `kFadedLevel`, the switch, and the rise of
     * `kRiseStep` a frame back to `kFullLevel`. */
    void updateMusic(f32 seconds);
    static constexpr s32 kFullLevel = 255;
    static constexpr s32 kFadedLevel = 3;
    static constexpr s32 kFadeStep = 3;
    static constexpr s32 kRiseStep = 8;
    static constexpr f32 kMusicRate = 30.0f;
    /** The area whose stream plays (sSelectStreamState), -1 before the music starts. */
    s32 musicArea() const { return m_playingArea; }
    /** The area asked for (sMusicSubIndex). */
    s32 musicRequest() const { return m_musicArea; }
    /** The music's level, kFullLevel but for a fade. */
    s32 musicLevel() const { return m_musicLevel; }
    /** The area a switch at the part's end waits for, -1 when none does. */
    s32 musicFollowing() const { return m_followArea; }
    const MusicAreas& musicAreas() const { return m_areas; }
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
    /** The stream's file name for an area's part: the stem, the area's letter past one area,
     * the part's number past one part. */
    std::string streamName(s32 area, s32 part) const;
    s32 partsOf(s32 area) const;
    /** Opens an area's parts, or nothing (with a warning) when one is missing. */
    std::vector<std::unique_ptr<StreamSource>> openArea(s32 area) const;
    /** One frame of AudioMusicVolUpdate: the switch, then the level's step. */
    void stepMusic();
    /** AudioSetupLevelStreams: switches the stream when the request differs in its way. */
    void setupStreams();
    /** Replaces the stream with an area's, from its first part, at the level of the moment. */
    void playArea(s32 area);
    f32 musicVolume() const;
    void stopMusic();

    SoundPlayer* m_output = nullptr;
    SoundSet m_common;
    SoundSet m_level;
    SoundSet m_ambient;
    SoundSet m_narrator;
    SoundSet m_narratorSecond;
    SoundSet m_promotions;
    AmbientSounds m_ambience; ///< cleared before its borrowed banks
    MusicAreas m_areas;
    std::array<std::optional<u32>, 2> m_steps{};
    std::optional<u32> m_pickup;
    std::string m_stream;
    s32 m_streamAreas = 1;
    std::array<s32, 8> m_streamParts{}; ///< a part count per area
    char m_realm = 'L';
    bool m_boss = false;
    const AssetLocator* m_assets = nullptr; ///< borrowed while the music runs
    std::shared_ptr<StreamPlaylist> m_playlist;
    f32 m_musicVolume = 1.0f;   ///< the level's
    bool m_musicOn = false;     ///< started and not stopped (sSelectStreamHandle held)
    bool m_musicSilent = false; ///< the area asked for had no stream: silent until another
    s32 m_musicArea = 0;        ///< sMusicSubIndex
    MusicSwitch m_musicSwitch = MusicSwitch::AtPartEnd; ///< sMusicSubState
    s32 m_playingArea = -1;                             ///< sSelectStreamState
    s32 m_followArea = -1;                              ///< the area waiting for the part's end
    usize m_followsTaken = 0; ///< the playlist's continuations seen through
    s32 m_musicLevel = kFullLevel;
    f32 m_musicFrames = 0.0f; ///< game time owed to the music, in its frames
    bool m_bossAwake = false;
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
