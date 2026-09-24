#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** A level's fog settings (the GameCube build leaves fog off; kept for completeness). */
struct LevelFog {
    u8 type = 0;
    std::array<u8, 3> color{255, 255, 255};
    f32 intensity = 0.0f;
    f32 density = 0.0f;
    f32 min = 0.0f;
    f32 max = 0.0f;
    f32 near = 0.0f;
    f32 far = 0.0f;
};

/** One level of a realm: its names, which camera and audio records it uses, and its light. */
/** A level's tuning, as its record holds it: a zero among those after `difficulty` stands
 * for the difficulty itself (`enemyMissileSpeed` for one). */
struct LevelTuningRecord {
    static constexpr usize kCount = 17;
    /** In the record's order from +0x9C: the player level the level is meant for, the
     * experience and damage multipliers, the difficulty, then the enemies', generators' and
     * traps' scales. */
    std::array<f32, kCount> values{};
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
    u32 flags = 0;
    std::string name;      ///< up to four characters, "L1"
    std::string title;     ///< "Tower"
    std::string audioBank; ///< usually empty; the audio record names the bank
    std::string movie;
    s32 bossType = 0;
    std::array<s16, 6> enemyTypes{}; ///< rows of the realm's enemies the level uses; -1 none
    s16 cameraIndex = -1;
    s16 audioIndex = -1;
    s16 mapIndex = -1;
    s16 bossCameraIndex = -1; ///< which of the realm's boss cameras the fight uses
    s16 rune = 0;
    s16 legend = 0;
    s16 maxEnemies = 0;
    f32 musicVolume = 0.0f;
    f32 soundVolume = 0.0f;
    LevelTuningRecord tuning;
    std::array<s32, 3> shopMaxima{};         ///< gold, kills, experience pile scales
    f32 ambient = 1.0f;                      ///< grey ambient light
    Vec3 lightDirection{-0.3f, -1.4f, 1.0f}; ///< the way the light travels
    Vec3 lightColor{1.0f, 1.0f, 1.0f};
    f32 lightIntensity = 1.0f;
    LevelFog fog;
};

/** How the follow camera behaves in a level. */
struct CameraRecord {
    s16 direction = 0;
    s16 pitchDirection = 0;
    f32 dp = 0.0f;
    f32 minPitch = 0.0f;
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    u8 limits = 0;
    u8 startEvent = 0;
    s16 attentionCamera = -1;
    f32 attention = 0.0f;
    f32 radiusMin = 0.0f;
    f32 radiusMax = 0.0f;
    s16 enemyMax = 0;
    s16 specialRadius = 0;
    f32 maxPitch = 0.0f;
    f32 pitchSub = 0.0f;
    f32 pitchMul = 0.0f;
    f32 pitchAdd = 0.0f;
    f32 distMulAdd = 0.0f;
    f32 distMulFactor = 0.0f;
    f32 distMulMin = 0.0f;
    f32 distMulMax = 0.0f;
    f32 smooth = 0.0f;
    f32 minYaw = 0.0f;
    f32 maxYaw = 0.0f;
    f32 bossRadiusMin = 0.0f;
    f32 bossRadiusMax = 0.0f;
};

/** How the camera frames a boss fight: how far it may swing about the party's line to the
 * boss, how far it stands from what it watches, how steep it looks, and where about the
 * boss (or the key it drops, or the wizard) it looks. */
struct BossCameraRecord {
    u32 flags = 0;
    f32 maxYaw = 0.0f;
    f32 cosMaxYaw = 0.0f;
    f32 minDistance = 0.0f;
    f32 minPlayerDistance = 0.0f;
    f32 maxDistance = 0.0f;
    f32 maxPlayerDistance = 0.0f;
    f32 minPitch = 0.0f;
    f32 maxPitch = 0.0f;
    Vec3 minAttention{0.0f, 0.0f, 0.0f}; ///< the look point's offset from the boss, near
    Vec3 maxAttention{0.0f, 0.0f, 0.0f}; ///< and far
    Vec3 keyAttention{0.0f, 0.0f, 0.0f}; ///< from the key it drops
    Vec3 wizardAttention{0.0f, 0.0f, 0.0f};
};

/** A level's sound bank, music stream and the sounds it plays on entry and on hits. */
struct AudioRecord {
    std::string bank;
    s16 enterSound = -1;
    s16 hitSound = -1;
    s32 nameSound = -1;
    std::string stream; ///< under STREAMS, without its extension
    s16 areas = 0;
    s16 stereo = 0;
    std::array<s16, 8> parts{};
};

/** A sound the realm looks up by name when it loads. */
struct SoundRecord {
    std::string name;
    s16 volume = 0;
    s16 priority = 0;
};

/** A realm's data wad: the levels of one world and the records they share. */
/** One kind of enemy a realm keeps: which, of what class (1 small, 2 medium, 3 large, 4 the
 * medium's second row, 5 a critter, 9 the boss), and its sound stream. */
struct WorldEnemyRecord {
    s32 kind = -1;
    s32 subtype = 0;
    std::string stream;
};

struct WorldDataFile {
    static constexpr usize kLevelSize = 0x10C;
    static constexpr usize kEnemySize = 0x18;
    static constexpr usize kLevelEnemyCount = 6;
    static constexpr usize kCameraSize = 0x6C;
    static constexpr usize kAudioSize = 0x3C;
    static constexpr usize kSoundSize = 0x18;
    static constexpr usize kBossCameraSize = 0x54;

    u32 realm = 0;
    std::string prefix; ///< the level folders' name without their number, "levelL"
    std::vector<WorldEnemyRecord> enemies;
    std::vector<LevelRecord> levels;
    std::vector<CameraRecord> cameras;
    std::vector<BossCameraRecord> bossCameras;
    std::vector<AudioRecord> audio;
    std::vector<SoundRecord> sounds;
    std::vector<std::array<Vec2, 9>> maps; ///< destination glow, then eight route dashes

    /** Parses a little-endian WDATA wad; throws FormatError. */
    static WorldDataFile parse(std::span<const u8> bytes);
};

} // namespace gdl::formats
