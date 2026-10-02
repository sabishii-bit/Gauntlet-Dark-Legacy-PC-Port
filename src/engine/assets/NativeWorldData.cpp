#include <exception>
#include <utility>

#include "engine/assets/WorldData.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "formats/WorldDataWad.h"

namespace gdl {
namespace {

LevelTuning nativeTuning(const formats::LevelTuningRecord& record) {
    const auto& v = record.values;
    const auto orOne = [](f32 value) { return value != 0 ? value : 1.0f; };
    LevelTuning result;
    result.playerLevel = v[0];
    result.experience = orOne(v[1]);
    result.damage = orOne(v[2]);
    result.difficulty = orOne(v[3]);
    const auto scaled = [&](usize i) { return v[i] != 0 ? v[i] : result.difficulty; };
    result.enemyHealth = scaled(4);
    result.enemySpeed = scaled(5);
    result.enemySight = scaled(6);
    // +0xB8's enemyAttack multiplier has no runtime consumer yet, in either loader.
    result.enemyDamage = scaled(8);
    result.enemyMissileRate = scaled(9);
    result.enemyMissileSpeed = orOne(v[10]);
    result.enemyMissileAim = scaled(11);
    result.generatorHealth = scaled(12);
    result.generatorRate = scaled(13);
    result.generatorMost = scaled(14);
    result.trapRate = scaled(15);
    result.trapDamage = scaled(16);
    return result;
}

BossCameraInfo nativeBossCamera(const formats::BossCameraRecord& from) {
    BossCameraInfo result;
    result.flags = from.flags;
    result.maxYaw = from.maxYaw;
    result.cosMaxYaw = from.cosMaxYaw;
    result.minDistance = from.minDistance;
    result.minPlayerDistance = from.minPlayerDistance;
    result.maxDistance = from.maxDistance;
    result.maxPlayerDistance = from.maxPlayerDistance;
    result.minPitch = from.minPitch;
    result.maxPitch = from.maxPitch;
    result.minAttention = from.minAttention;
    result.maxAttention = from.maxAttention;
    result.keyAttention = from.keyAttention;
    result.wizardAttention = from.wizardAttention;
    return result;
}

} // namespace

bool WorldData::loadNative(const std::filesystem::path& file) {
    try {
        auto source = formats::WorldDataFile::parse(readFile(file));
        m_realm = source.realm;
        m_prefix = std::move(source.prefix);
        for (const auto& from : source.levels) {
            LevelInfo level;
            level.selectionFlags = from.selectionFlags;
            level.flags = from.flags;
            level.timeLimit = from.timeLimit;
            level.name = from.name;
            level.title = from.title;
            level.audioBank = from.audioBank;
            level.movie = from.movie;
            if (from.mapIndex >= 0 && static_cast<usize>(from.mapIndex) < source.maps.size()) {
                const auto& points = source.maps[static_cast<usize>(from.mapIndex)];
                level.mapPoints.assign(points.begin(), points.end());
            }
            level.cameraIndex = from.cameraIndex;
            level.audioIndex = from.audioIndex;
            level.maxEnemies = from.maxEnemies;
            level.bossType = from.bossType;
            level.rune = from.rune;
            level.legend = from.legend;
            if (from.bossCameraIndex >= 0 &&
                static_cast<usize>(from.bossCameraIndex) < source.bossCameras.size()) {
                level.bossCamera =
                    nativeBossCamera(source.bossCameras[static_cast<usize>(from.bossCameraIndex)]);
            }
            for (const auto index : from.enemyTypes) {
                if (index >= 0 && static_cast<usize>(index) < source.enemies.size()) {
                    const auto& enemy = source.enemies[static_cast<usize>(index)];
                    level.enemies.push_back({enemy.kind, enemy.subtype, enemy.stream});
                }
            }
            level.musicVolume = from.musicVolume;
            level.soundVolume = from.soundVolume;
            level.shopMaxima = from.shopMaxima;
            level.tuning = nativeTuning(from.tuning);
            level.ambient = from.ambient;
            level.lightDirection = from.lightDirection;
            level.lightColor = from.lightColor;
            level.lightIntensity = from.lightIntensity;
            m_levels.push_back(std::move(level));
        }
        for (const auto& from : source.cameras) {
            LevelCameraInfo camera;
            camera.minPitch = from.minPitch;
            camera.maxPitch = from.maxPitch;
            camera.boundsMin = from.boundsMin;
            camera.boundsMax = from.boundsMax;
            camera.attention = from.attention;
            camera.radiusMin = from.radiusMin;
            camera.radiusMax = from.radiusMax;
            camera.smooth = from.smooth;
            camera.minYaw = from.minYaw;
            camera.maxYaw = from.maxYaw;
            m_cameras.push_back(camera);
        }
        for (auto& from : source.audio) {
            LevelAudioInfo audio;
            audio.bank = std::move(from.bank);
            audio.stream = std::move(from.stream);
            audio.enterSound = from.enterSound;
            audio.hitSound = from.hitSound;
            audio.nameSound = from.nameSound;
            audio.areas = from.areas;
            for (usize i = 0; i < audio.parts.size(); ++i) {
                audio.parts[i] = from.parts[i];
            }
            m_audio.push_back(std::move(audio));
        }
        for (auto& sound : source.sounds) {
            m_sounds.push_back(std::move(sound.name));
        }
        return loaded();
    } catch (const std::exception& e) {
        log::warn("Native world data {}: {}", file.string(), e.what());
        *this = WorldData{};
        return false;
    }
}

} // namespace gdl
