#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** One level of a realm as its unpacked data describes it. */
/** How hard a level is on the party: what scales its traps and the harm it does. The file
 * leaves a scale at zero to mean the level's difficulty. */
struct LevelTuning {
    f32 difficulty = 1.0f;
    f32 damage = 1.0f;     ///< scales every hurt over a point
    f32 trapRate = 1.0f;   ///< how fast its traps cycle
    f32 trapDamage = 1.0f; ///< scales what its traps and blasts do

    /** How long a trap's times run for a game whose difficulty setting scales by `gain`:
     * the faster the rate, the shorter. */
    f32 trapTimeScale(f32 gain) const;
    /** What a trap's or a blast's damage is multiplied by. */
    f32 trapDamageScale(f32 gain) const { return trapDamage * gain; }
};

struct LevelInfo {
    std::string name;  ///< "L1"
    std::string title; ///< "Tower"
    std::string audioBank;
    std::string movie;
    s32 cameraIndex = -1;
    s32 audioIndex = -1;
    f32 musicVolume = 1.0f;
    f32 soundVolume = 1.0f;
    LevelTuning tuning;
    f32 ambient = 1.0f;                      ///< grey ambient light
    Vec3 lightDirection{-0.3f, -1.4f, 1.0f}; ///< the way the light travels
    Vec3 lightColor{1.0f, 1.0f, 1.0f};
    f32 lightIntensity = 1.0f;
};

/** How a level's follow camera is set. */
struct LevelCameraInfo {
    f32 minPitch = 0.0f;
    f32 maxPitch = 0.0f;
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    f32 attention = 0.0f;
    f32 radiusMin = 0.0f;
    f32 radiusMax = 0.0f;
    f32 smooth = 0.0f;
    f32 minYaw = 0.0f;
    f32 maxYaw = 0.0f;
};

/** A level's sound bank, music stream and the sounds it plays on entry and on hits. */
struct LevelAudioInfo {
    std::string bank;
    std::string stream;
    s32 enterSound = -1;
    s32 hitSound = -1;
    s32 nameSound = -1;
};

/** A realm's unpacked data (`wdata/<REALM>.json`): its levels and the records they share. */
class WorldData {
public:
    /** Reads the file; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file);
    bool loaded() const { return !m_levels.empty(); }

    u32 realm() const { return m_realm; }
    const std::string& prefix() const { return m_prefix; }
    const std::vector<LevelInfo>& levels() const { return m_levels; }
    /** The name of the realm's sound `index` (what a level's enterSound and hitSound
     * index); empty when out of range. */
    std::string_view soundName(s32 index) const;
    /** The level called `name`, or null. */
    const LevelInfo* level(std::string_view name) const;
    /** The camera or audio record at `index`, or null when out of range. */
    const LevelCameraInfo* camera(s32 index) const;
    const LevelAudioInfo* audio(s32 index) const;

private:
    u32 m_realm = 0;
    std::string m_prefix;
    std::vector<LevelInfo> m_levels;
    std::vector<LevelCameraInfo> m_cameras;
    std::vector<LevelAudioInfo> m_audio;
    std::vector<std::string> m_sounds;
};

} // namespace gdl
