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
#include "game/world/VoiceQueue.h"

namespace gdl::game {

/** What a character's foot comes down on: the original's step kinds (pmotion.c 2919). */
enum class Footing : u8 { Rock, Wood, Stair, Metal, Water };

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
    /** Binds the level's ambient loops, its music zones and its objects' flags. */
    void bindAmbience(const WorldLayout& layout, const WorldScene* world = nullptr);
    /** ItemVisible's joined population, independent of active listening positions. */
    void setPlayerCount(s32 count);
    /** Places the loops; `ducked` holds every one that plays at kDuckedLevel instead (Sumner
     * speaking or a trigger camera running: sounds.c 909). */
    void updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear, f32 volume,
                        bool ducked = false, const WorldScene* world = nullptr);
    /** Stop proximity loops while gameplay is held; the next gameplay update restores them.
     * Leaves music, narration and menu sounds under their own controls. */
    void pauseAmbience();
    static constexpr f32 kDuckedLevel = 16.0f / 255.0f;
    /** Lets the zones ask for the area holding the party (items.c 4514-4522, 4733-4736). */
    void updateMusicAreas(std::span<const Vec3> listeners, const WorldScene* world = nullptr);
    /** Starts the first area's stream at `volume`, the level's; `assets` is borrowed for the
     * other areas' streams until the music stops. */
    void startMusic(const AssetLocator* assets, f32 volume);
    /** Asks for an area's stream and how to go over to it (sMusicSubIndex and sMusicSubState);
     * `updateMusic` carries it out. A level with one area ignores it. */
    void selectMusicArea(s32 area, MusicSwitch how);
    /** Told every frame whether the boss is awake: its waking asks for the second area with
     * a fade (BossActivate, boss.c 695). */
    void bossAwake(bool awake);
    /** Holds the music at `scale` of its level for `seconds` (sMusicVolScale, running to
     * sMusicFadeCur): a runestone found halves it for the sting's length less a second
     * (sounds_evt.c 1948). An area's fade takes precedence. */
    void duckMusic(f32 seconds, f32 scale);
    static constexpr f32 kRuneMusicScale = 0.5f;
    static constexpr f32 kRuneMusicLead = 1.0f; ///< seconds before the sting ends that it lifts
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
    /** The music's level, kFullLevel but for a fade or a duck. */
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

    /** Search level, common, then ambient banks; a broken first match stays silent.
     * With `after`, wait for that voice to finish before starting this one. */
    SoundHandle playNamed(std::string_view name, f32 volume = 1.0f, SoundHandle after = kNoSound);
    bool isPlaying(SoundHandle handle) const;
    /** A one-shot attenuated from the nearest player and panned from the latest ambience ear. */
    SoundHandle playAt(std::string_view name, const Vec3& position, f32 playerDistance,
                       f32 volume = 1.0f);
    static constexpr f32 kSplashLevel = 180.0f; ///< byte volume of a projectile's water impact
    SoundHandle playFrom(SoundSet& bank, std::string_view name, f32 volume = 1.0f);
    SoundHandle playPromotion(std::string_view name, SoundHandle after = kNoSound);
    /** How long a named sound of the level's banks plays, in seconds; nought when unknown. */
    f32 lengthOf(std::string_view name);
    enum class Narrator : u8 { Primary, Either };
    SoundHandle narrate(std::string_view name, Narrator which = Narrator::Either,
                        SoundHandle after = kNoSound);

    /** The narrator's queue (sndFxQueAddEx's second mode): lines play one after another, at
     * most sixteen waiting. An announcement asks for room first, giving how long it is
     * willing to wait behind what is already queued (never refused when negative); once let in,
     * its parts are queued in turn. */
    static constexpr usize kMostNarration = VoiceQueue::kMost;
    static constexpr f32 kAlwaysRoom = VoiceQueue::kAlwaysRoom;
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
    void holdNarration(bool held) { m_narration.hold(held); }
    /** Advances both queues' clocks, letting finished lines go. */
    void updateNarration(f32 seconds);
    /** Seconds of queued narration still to come. */
    f64 narrationBacklog() const;

    /** The characters' queue (sndFxQueAddEx's first mode, sndfx.c 241): the pain,
     * eating and theft cries wait their turn behind one another, each willing to wait a
     * second (AudioPlayerPain, AudioPlayerSeverePain, AudioPlayerEatFood, fn_8009F748). */
    static constexpr f32 kBarkWait = 1.0f;
    static constexpr f32 kPainVolume = 224.0f / 255.0f; ///< a pain cry's (AudioPlayerPain)
    static constexpr f32 kBarkVolume = 192.0f / 255.0f; ///< every other bark's
    bool barkRoom(f32 maxWait = kBarkWait) const;
    /** Queues a cry of a character's bank, or one of the level's banks (Pojo's, in COMMON);
     * kNoSound when turned away, missing or full. */
    SoundHandle bark(SoundSet& bank, std::string_view name, f32 volume = kBarkVolume,
                     f32 maxWait = kBarkWait);
    SoundHandle barkNamed(std::string_view name, f32 volume = kBarkVolume, f32 maxWait = kBarkWait);
    f64 barkBacklog() const { return m_barks.backlog(); }

    void playPickup();
    /** A foot coming down on `footing` (fn_8009EFCC: the COMMON bank's S_STEP<kind>1/2 at
     * kStepVolume), heard from `distance` off the nearest standing player. */
    void playFootstep(bool second, Footing footing = Footing::Rock, f32 distance = 0.0f);
    static Footing footingOf(u32 floorFlags, u32 armorFlags, bool inWater = false);
    static constexpr f32 kStepVolume = 127.0f / 255.0f;
    /** How loud a sound `distance` from the nearest standing player is (sndFxPlay3DAtten):
     * whole within kAttenuationNear, nothing past kAttenuationFar, straight between. */
    static f32 attenuation(f32 distance);
    static constexpr f32 kAttenuationNear = 20.0f;
    static constexpr f32 kAttenuationFar = 70.0f;
    /** S_ENTRANCE as the party materialises (fn_8009D288 beside StartEnterFX, gauntworld.c
     * 1131), at kEntranceVolume. */
    void playEntrance();
    /** A serpent trap waking within forty units of the camera's attention. */
    SoundHandle playSerpent(const Vec3& position, const Vec3& attention, f32 playerDistance,
                            const AmbientEar& ear);
    static constexpr f32 kEntranceVolume = 224.0f / 255.0f;
    /** The narrator's word as the level's title lands (camera.c 5047-5057): S_SHOTSSTUN for a
     * level flagged kStunLevel (willing to wait ten seconds), S_GRAB for one flagged kGrabLevel
     * (one second), both from VOICE1. */
    void announceTitle(u32 levelFlags);
    /** Runestone finder lines (fn_8009FF54 / fn_8009FFA4): VOICE1, volume 224,
     * at most three seconds behind queued narration; silent while Sumner holds it. */
    SoundHandle announceRune(bool nearby, const Vec3& attention, const AmbientEar& ear);
    static constexpr u32 kStunLevel = 1;
    static constexpr u32 kGrabLevel = 4;
    static constexpr f32 kStunWait = 10.0f;
    static constexpr f32 kGrabWait = 1.0f;
    /** The exit's flame (S_EXITFLAME, fn_8009D610): a loop at kLoopVolume while a standing
     * player stands still on an exit, panned to the first who does; stopped once nobody is. */
    void updateExitFlame(const std::optional<Vec3>& stander, const AmbientEar& ear);
    bool exitFlameOn() const { return m_exitFlame.handle != kNoSound; }
    static constexpr f32 kLoopVolume = 224.0f / 255.0f;
    /** The hourglass (S_HOURGLASS, AudioAmbientUpdate): a loop at kHourglassVolume panned to
     * the first standing player whose time stop runs; stopped once none does. */
    void updateHourglass(const std::optional<Vec3>& wearer, const AmbientEar& ear);
    bool hourglassOn() const { return m_hourglass.handle != kNoSound; }
    static constexpr f32 kHourglassVolume = 127.0f / 255.0f;
    void speakOverScroll(std::string_view name);
    void stopVoice();
    void stop(SoundHandle handle);
    /** A target opening (fn_80062A00): slots under kMotionSlot sound their pair (`playOpening`);
     * kMotionSlot is the realm's S_TRAP as the motion starts and again as it ends, and
     * kMotionSlot + 1 the pyramid's S_QUAKEC or the sky's S_ELVCNNK once for a target whose
     * flags carry kMotionFlags, all at kMotionVolume and never where a boss is fought. */
    void opening(const TriggerOpening& event);
    void settled(const TriggerOpening& event);
    static constexpr s32 kMotionSlot = 10;
    static constexpr u32 kMotionFlags = 0x00C00000;
    static constexpr f32 kMotionVolume = 224.0f / 255.0f;

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
    /** A loop placed for as long as something asks for it. */
    struct PlacedLoop {
        std::string_view name;
        f32 volume = 1.0f;
        SoundHandle handle = kNoSound;
    };
    void placeLoop(PlacedLoop& loop, const std::optional<Vec3>& spot, const AmbientEar& ear);
    void stopLoop(PlacedLoop& loop);
    SoundHandle track(SoundHandle handle);
    SoundHandle playOpening(s32 slot, bool settled);
    void playMotion(s32 slot, s32 target, bool settled);
    /** The bank of the level's own that holds `name`, if one does: level, common, ambient. */
    SoundSet* bankOf(std::string_view name, u32& sound);
    SoundHandle queue(SoundSet& bank, u32 sound);
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
    AmbientEar m_ear;
    MusicAreas m_areas;
    std::vector<u32> m_objectFlags; ///< the level's objects' flags, by object
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
    f32 m_musicDuck = 0.0f;   ///< seconds the duck has left to run
    f32 m_musicDuckScale = 1.0f;
    bool m_bossAwake = false;
    SoundHandle m_music = kNoSound;
    SoundHandle m_voice = kNoSound;
    std::vector<Opening> m_openings;
    PlacedLoop m_exitFlame{"S_EXITFLAME", kLoopVolume};
    PlacedLoop m_hourglass{"S_HOURGLASS", kHourglassVolume};
    std::vector<SoundHandle> m_voices; ///< includes queued lines; pruned as new sounds start
    VoiceQueue m_narration;
    VoiceQueue m_barks;
};

} // namespace gdl::game
