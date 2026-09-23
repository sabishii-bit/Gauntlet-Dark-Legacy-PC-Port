#pragma once

#include <array>
#include <filesystem>
#include <numbers>
#include <optional>
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
    f32 playerLevel = 0.0f;       ///< the level the place is meant for; none when nought
    f32 experience = 1.0f;        ///< scales what is won there
    f32 damage = 1.0f;            ///< scales every hurt over a point
    f32 trapRate = 1.0f;          ///< how fast its traps cycle
    f32 trapDamage = 1.0f;        ///< scales what its traps and blasts do
    f32 enemyHealth = 1.0f;       ///< scales what its enemies can take
    f32 enemySpeed = 1.0f;        ///< and how fast they go
    f32 enemySight = 1.0f;        ///< and how far they see
    f32 enemyDamage = 1.0f;       ///< and what they deal
    f32 generatorHealth = 1.0f;   ///< scales what a generator can take
    f32 generatorRate = 1.0f;     ///< and how quickly it breeds
    f32 generatorMost = 1.0f;     ///< and how many it keeps out at once
    f32 enemyMissileSpeed = 1.0f; ///< scales how fast what they throw flies

    /** How long a trap's times run for a game whose difficulty setting scales by `gain`:
     * the faster the rate, the shorter. */
    f32 trapTimeScale(f32 gain) const;
    /** What a trap's or a blast's damage is multiplied by. */
    f32 trapDamageScale(f32 gain) const { return trapDamage * gain; }
    /** What experience won here by a character of `level` is multiplied by: the place's own
     * scale, less the further the character is past the level it is meant for. */
    f32 experienceScale(s32 level) const;
    /** How far the enemies see, how fast they go, and how many a generator breeds and how
     * quickly, all grow with the difficulty setting's `gain`; what they and the generators can
     * take and deal does not. */
    f32 enemySpeedScale(f32 gain) const { return enemySpeed * gain; }
    f32 enemySightScale(f32 gain) const { return enemySight * gain; }
    f32 generatorRateScale(f32 gain) const { return generatorRate * gain; }
    f32 generatorMostScale(f32 gain) const { return generatorMost * gain; }
};

/** One of the kinds a level holds: which, and of what class (1 small, 2 medium, 3 large,
 * 4 the medium's second row, 5 a critter, 9 the boss). */
struct LevelEnemy {
    s32 kind = -1;
    s32 subtype = 0;
};

/** How the camera frames a boss fight: how far about the party's line to the boss it may
 * swing, how far it stands and how steep it looks, and where about the boss it looks. */
struct BossCameraInfo {
    u32 flags = 0;
    f32 maxYaw = std::numbers::pi_v<f32>;
    f32 cosMaxYaw = -1.0f;
    f32 minDistance = 25.0f;
    f32 minPlayerDistance = 25.0f;
    f32 maxDistance = 75.0f;
    f32 maxPlayerDistance = 30.0f;
    f32 minPitch = 0.3f;
    f32 maxPitch = 0.45f;
    Vec3 minAttention{0.0f, 0.0f, 0.0f}; ///< the look point's offset from the boss, close up
    Vec3 maxAttention{0.0f, 0.0f, 0.0f}; ///< and at its furthest
    Vec3 keyAttention{0.0f, 0.0f, 0.0f}; ///< from the key it drops
    Vec3 wizardAttention{0.0f, 0.0f, 0.0f};
};

struct LevelInfo {
    std::string name;  ///< "L1"
    std::string title; ///< "Tower"
    std::string audioBank;
    std::string movie;
    s32 cameraIndex = -1;
    s32 audioIndex = -1;
    s32 maxEnemies = 25; ///< how many enemies the place keeps about at once
    s32 bossType = -1;   ///< the kind of its boss, none under nought
    s32 rune = 0;        ///< the runestone it holds, from one; none at nought
    s32 legend = 0;      ///< the realm whose legend item it holds; none at nought
    std::optional<BossCameraInfo> bossCamera; ///< how its boss fight is framed, when it has one
    std::vector<LevelEnemy> enemies;          ///< its roster, from the realm's
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
    s32 areas = 1;
    std::array<s32, 8> parts{};
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
