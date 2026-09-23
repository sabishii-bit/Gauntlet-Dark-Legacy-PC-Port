#pragma once

#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl {

/** One level of a realm as its unpacked data describes it. */
/** How hard a level is on the party: what scales its traps and the harm it does. The file
 * leaves a scale at zero to mean the level's difficulty. */
struct LevelTuning {
    float difficulty = 1.0f;
    float playerLevel = 0.0f;       ///< the level the place is meant for; none when nought
    float experience = 1.0f;        ///< scales what is won there
    float damage = 1.0f;            ///< scales every hurt over a point
    float trapRate = 1.0f;          ///< how fast its traps cycle
    float trapDamage = 1.0f;        ///< scales what its traps and blasts do
    float enemyHealth = 1.0f;       ///< scales what its enemies can take
    float enemySpeed = 1.0f;        ///< and how fast they go
    float enemySight = 1.0f;        ///< and how far they see
    float enemyDamage = 1.0f;       ///< and what they deal
    float generatorHealth = 1.0f;   ///< scales what a generator can take
    float generatorRate = 1.0f;     ///< and how quickly it breeds
    float generatorMost = 1.0f;     ///< and how many it keeps out at once
    float enemyMissileSpeed = 1.0f; ///< scales how fast what they throw flies

    /** How long a trap's times run for a game whose difficulty setting scales by `gain`:
     * the faster the rate, the shorter. */
    float trapTimeScale(float gain) const;
    /** What a trap's or a blast's damage is multiplied by. */
    float trapDamageScale(float gain) const { return trapDamage * gain; }
    /** What experience won here by a character of `level` is multiplied by: the place's own
     * scale, less the further the character is past the level it is meant for. */
    float experienceScale(std::int32_t level) const;
    /** How far the enemies see, how fast they go, and how many a generator breeds and how
     * quickly, all grow with the difficulty setting's `gain`; what they and the generators can
     * take and deal does not. */
    float enemySpeedScale(float gain) const { return enemySpeed * gain; }
    float enemySightScale(float gain) const { return enemySight * gain; }
    float generatorRateScale(float gain) const { return generatorRate * gain; }
    float generatorMostScale(float gain) const { return generatorMost * gain; }
};

/** One of the kinds a level holds: which, and of what class (1 small, 2 medium, 3 large,
 * 4 the medium's second row, 5 a critter, 9 the boss). */
struct LevelEnemy {
    std::int32_t kind = -1;
    std::int32_t subtype = 0;
};

/** How the camera frames a boss fight: how far about the party's line to the boss it may
 * swing, how far it stands and how steep it looks, and where about the boss it looks. */
struct BossCameraInfo {
    std::uint32_t flags = 0;
    float maxYaw = std::numbers::pi_v<float>;
    float cosMaxYaw = -1.0f;
    float minDistance = 25.0f;
    float minPlayerDistance = 25.0f;
    float maxDistance = 75.0f;
    float maxPlayerDistance = 30.0f;
    float minPitch = 0.3f;
    float maxPitch = 0.45f;
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
    std::int32_t cameraIndex = -1;
    std::int32_t audioIndex = -1;
    std::int32_t maxEnemies = 25; ///< how many enemies the place keeps about at once
    std::int32_t bossType = -1;   ///< the kind of its boss, none under nought
    std::int32_t rune = 0;        ///< the runestone it holds, from one; none at nought
    std::int32_t legend = 0;      ///< the realm whose legend item it holds; none at nought
    std::optional<BossCameraInfo> bossCamera; ///< how its boss fight is framed, when it has one
    std::vector<LevelEnemy> enemies;          ///< its roster, from the realm's
    float musicVolume = 1.0f;
    float soundVolume = 1.0f;
    LevelTuning tuning;
    float ambient = 1.0f;                    ///< grey ambient light
    Vec3 lightDirection{-0.3f, -1.4f, 1.0f}; ///< the way the light travels
    Vec3 lightColor{1.0f, 1.0f, 1.0f};
    float lightIntensity = 1.0f;
};

/** How a level's follow camera is set. */
struct LevelCameraInfo {
    float minPitch = 0.0f;
    float maxPitch = 0.0f;
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    float attention = 0.0f;
    float radiusMin = 0.0f;
    float radiusMax = 0.0f;
    float smooth = 0.0f;
    float minYaw = 0.0f;
    float maxYaw = 0.0f;
};

/** A level's sound bank, music stream and the sounds it plays on entry and on hits. */
struct LevelAudioInfo {
    std::string bank;
    std::string stream;
    std::int32_t enterSound = -1;
    std::int32_t hitSound = -1;
    std::int32_t nameSound = -1;
};

/** A realm's unpacked data (`wdata/<REALM>.json`): its levels and the records they share. */
class WorldData {
public:
    /** Reads the file; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file);
    bool loaded() const { return !m_levels.empty(); }

    std::uint32_t realm() const { return m_realm; }
    const std::string& prefix() const { return m_prefix; }
    const std::vector<LevelInfo>& levels() const { return m_levels; }
    /** The name of the realm's sound `index` (what a level's enterSound and hitSound
     * index); empty when out of range. */
    std::string_view soundName(std::int32_t index) const;
    /** The level called `name`, or null. */
    const LevelInfo* level(std::string_view name) const;
    /** The camera or audio record at `index`, or null when out of range. */
    const LevelCameraInfo* camera(std::int32_t index) const;
    const LevelAudioInfo* audio(std::int32_t index) const;

private:
    std::uint32_t m_realm = 0;
    std::string m_prefix;
    std::vector<LevelInfo> m_levels;
    std::vector<LevelCameraInfo> m_cameras;
    std::vector<LevelAudioInfo> m_audio;
    std::vector<std::string> m_sounds;
};

} // namespace gdl
