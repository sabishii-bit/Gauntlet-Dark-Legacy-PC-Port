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

    void open(const std::filesystem::path& root, SoundPlayer* output, const LevelAudioInfo* info);
    void bindAmbience(const WorldLayout& layout);
    void updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear, f32 volume);
    void startMusic(const AssetLocator* assets, f32 volume);
    /** Stop scene cues early in teardown, leaving ambient loops until close(). */
    void stopCues();
    /** Stops every voice started here before releasing its borrowed clips. */
    void close();

    /** Search level, common, then ambient banks; a broken first match stays silent. */
    SoundHandle playNamed(std::string_view name);
    SoundHandle playFrom(SoundSet& bank, std::string_view name);
    enum class Narrator : u8 { Primary, Either };
    SoundHandle narrate(std::string_view name, Narrator which = Narrator::Either,
                        SoundHandle after = kNoSound);
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

    SoundPlayer* m_output = nullptr;
    SoundSet m_common;
    SoundSet m_level;
    SoundSet m_ambient;
    SoundSet m_narrator;
    SoundSet m_narratorSecond;
    AmbientSounds m_ambience; ///< cleared before its borrowed banks
    std::array<std::optional<u32>, 2> m_steps{};
    std::optional<u32> m_pickup;
    std::string m_stream;
    SoundHandle m_music = kNoSound;
    SoundHandle m_voice = kNoSound;
    std::vector<Opening> m_openings;
    std::vector<SoundHandle> m_voices; ///< includes queued narration; pruned as new sounds start
};

} // namespace gdl::game
