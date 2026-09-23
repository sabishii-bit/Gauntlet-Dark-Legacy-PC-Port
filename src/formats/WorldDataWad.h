#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::formats {

/** A level's fog settings (the GameCube build leaves fog off; kept for completeness). */
struct LevelFog {
    std::uint8_t type = 0;
    std::array<std::uint8_t, 3> color{255, 255, 255};
    float intensity = 0.0f;
    float density = 0.0f;
    float min = 0.0f;
    float max = 0.0f;
    float near = 0.0f;
    float far = 0.0f;
};

/** One level of a realm: its names, which camera and audio records it uses, and its light. */
/** A level's tuning, as its record holds it: a zero among those after `difficulty` stands
 * for the difficulty itself (`enemyMissileSpeed` for one). */
struct LevelTuningRecord {
    static constexpr std::size_t kCount = 17;
    /** In the record's order from +0x9C: the player level the level is meant for, the
     * experience and damage multipliers, the difficulty, then the enemies', generators' and
     * traps' scales. */
    std::array<float, kCount> values{};
    static constexpr std::array<std::string_view, kCount> kNames{
        "playerLevel",      "experience",        "damage",
        "difficulty",       "enemyHealth",       "enemySpeed",
        "enemySight",       "enemyAttack",       "enemyDamage",
        "enemyMissileRate", "enemyMissileSpeed", "enemyMissileAim",
        "generatorHealth",  "generatorRate",     "generatorMost",
        "trapRate",         "trapDamage",
    };
};

struct LevelRecord {
    std::uint32_t flags = 0;
    std::string name;      ///< up to four characters, "L1"
    std::string title;     ///< "Tower"
    std::string audioBank; ///< usually empty; the audio record names the bank
    std::string movie;
    std::int32_t bossType = 0;
    std::array<std::int16_t, 6>
        enemyTypes{}; ///< rows of the realm's enemies the level uses; -1 none
    std::int16_t cameraIndex = -1;
    std::int16_t audioIndex = -1;
    std::int16_t mapIndex = -1;
    std::int16_t bossCameraIndex = -1; ///< which of the realm's boss cameras the fight uses
    std::int16_t rune = 0;
    std::int16_t legend = 0;
    std::int16_t maxEnemies = 0;
    float musicVolume = 0.0f;
    float soundVolume = 0.0f;
    LevelTuningRecord tuning;
    float ambient = 1.0f;                    ///< grey ambient light
    Vec3 lightDirection{-0.3f, -1.4f, 1.0f}; ///< the way the light travels
    Vec3 lightColor{1.0f, 1.0f, 1.0f};
    float lightIntensity = 1.0f;
    LevelFog fog;
};

/** How the follow camera behaves in a level. */
struct CameraRecord {
    std::int16_t direction = 0;
    std::int16_t pitchDirection = 0;
    float dp = 0.0f;
    float minPitch = 0.0f;
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    std::uint8_t limits = 0;
    std::uint8_t startEvent = 0;
    std::int16_t attentionCamera = -1;
    float attention = 0.0f;
    float radiusMin = 0.0f;
    float radiusMax = 0.0f;
    std::int16_t enemyMax = 0;
    std::int16_t specialRadius = 0;
    float maxPitch = 0.0f;
    float pitchSub = 0.0f;
    float pitchMul = 0.0f;
    float pitchAdd = 0.0f;
    float distMulAdd = 0.0f;
    float distMulFactor = 0.0f;
    float distMulMin = 0.0f;
    float distMulMax = 0.0f;
    float smooth = 0.0f;
    float minYaw = 0.0f;
    float maxYaw = 0.0f;
    float bossRadiusMin = 0.0f;
    float bossRadiusMax = 0.0f;
};

/** How the camera frames a boss fight: how far it may swing about the party's line to the
 * boss, how far it stands from what it watches, how steep it looks, and where about the
 * boss (or the key it drops, or the wizard) it looks. */
struct BossCameraRecord {
    std::uint32_t flags = 0;
    float maxYaw = 0.0f;
    float cosMaxYaw = 0.0f;
    float minDistance = 0.0f;
    float minPlayerDistance = 0.0f;
    float maxDistance = 0.0f;
    float maxPlayerDistance = 0.0f;
    float minPitch = 0.0f;
    float maxPitch = 0.0f;
    Vec3 minAttention{0.0f, 0.0f, 0.0f}; ///< the look point's offset from the boss, near
    Vec3 maxAttention{0.0f, 0.0f, 0.0f}; ///< and far
    Vec3 keyAttention{0.0f, 0.0f, 0.0f}; ///< from the key it drops
    Vec3 wizardAttention{0.0f, 0.0f, 0.0f};
};

/** A level's sound bank, music stream and the sounds it plays on entry and on hits. */
struct AudioRecord {
    std::string bank;
    std::int16_t enterSound = -1;
    std::int16_t hitSound = -1;
    std::int32_t nameSound = -1;
    std::string stream; ///< under STREAMS, without its extension
    std::int16_t areas = 0;
    std::int16_t stereo = 0;
    std::array<std::int16_t, 8> parts{};
};

/** A sound the realm looks up by name when it loads. */
struct SoundRecord {
    std::string name;
    std::int16_t volume = 0;
    std::int16_t priority = 0;
};

/** A realm's data wad: the levels of one world and the records they share. */
/** One kind of enemy a realm keeps: which, of what class (1 small, 2 medium, 3 large, 4 the
 * medium's second row, 5 a critter, 9 the boss), and its sound stream. */
struct WorldEnemyRecord {
    std::int32_t kind = -1;
    std::int32_t subtype = 0;
    std::string stream;
};

struct WorldDataFile {
    static constexpr std::size_t kLevelSize = 0x10C;
    static constexpr std::size_t kEnemySize = 0x18;
    static constexpr std::size_t kLevelEnemyCount = 6;
    static constexpr std::size_t kCameraSize = 0x6C;
    static constexpr std::size_t kAudioSize = 0x3C;
    static constexpr std::size_t kSoundSize = 0x18;
    static constexpr std::size_t kBossCameraSize = 0x54;

    std::uint32_t realm = 0;
    std::string prefix; ///< the level folders' name without their number, "levelL"
    std::vector<WorldEnemyRecord> enemies;
    std::vector<LevelRecord> levels;
    std::vector<CameraRecord> cameras;
    std::vector<BossCameraRecord> bossCameras;
    std::vector<AudioRecord> audio;
    std::vector<SoundRecord> sounds;

    /** Parses a little-endian WDATA wad; throws FormatError. */
    static WorldDataFile parse(std::span<const std::uint8_t> bytes);
};

} // namespace gdl::formats
