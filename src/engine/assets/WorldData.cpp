#include "engine/assets/WorldData.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

Vec3 readVec3(const nlohmann::json& array, const Vec3& fallback) {
    if (!array.is_array() || array.size() < 3) {
        return fallback;
    }
    return Vec3{array.at(0).get<float>(), array.at(1).get<float>(), array.at(2).get<float>()};
}

LevelInfo parseLevel(const nlohmann::json& json, std::span<const LevelEnemy> roster) {
    LevelInfo level;
    level.name = json.value("name", std::string{});
    level.title = json.value("title", std::string{});
    level.audioBank = json.value("audioBank", std::string{});
    level.movie = json.value("movie", std::string{});
    level.cameraIndex = json.value("cameraIndex", -1);
    level.audioIndex = json.value("audioIndex", -1);
    level.maxEnemies = json.value("maxEnemies", 25);
    level.bossType = json.value("bossType", -1);
    level.rune = json.value("rune", 0);
    level.legend = json.value("legend", 0);
    for (const auto row : json.value("enemyTypes", std::vector<std::int32_t>{})) {
        if (row >= 0 && static_cast<std::size_t>(row) < roster.size()) {
            level.enemies.push_back(roster[static_cast<std::size_t>(row)]);
        }
    }
    level.musicVolume = json.value("musicVolume", 1.0f);
    level.soundVolume = json.value("soundVolume", 1.0f);
    if (const auto tuning = json.find("tuning"); tuning != json.end() && tuning->is_object()) {
        // A zero stands for the difficulty, itself one when it is zero.
        const float difficulty = tuning->value("difficulty", 0.0f);
        level.tuning.difficulty = difficulty != 0.0f ? difficulty : 1.0f;
        const auto scaled = [&](const char* key) {
            const float value = tuning->value(key, 0.0f);
            return value != 0.0f ? value : level.tuning.difficulty;
        };
        const float damage = tuning->value("damage", 0.0f);
        level.tuning.damage = damage != 0.0f ? damage : 1.0f;
        level.tuning.playerLevel = tuning->value("playerLevel", 0.0f);
        const float experience = tuning->value("experience", 0.0f);
        level.tuning.experience = experience != 0.0f ? experience : 1.0f;
        level.tuning.trapRate = scaled("trapRate");
        level.tuning.trapDamage = scaled("trapDamage");
        level.tuning.enemyHealth = scaled("enemyHealth");
        level.tuning.enemySpeed = scaled("enemySpeed");
        level.tuning.enemySight = scaled("enemySight");
        level.tuning.enemyDamage = scaled("enemyDamage");
        level.tuning.generatorHealth = scaled("generatorHealth");
        level.tuning.generatorRate = scaled("generatorRate");
        level.tuning.generatorMost = scaled("generatorMost");
        const float missileSpeed = tuning->value("enemyMissileSpeed", 0.0f);
        level.tuning.enemyMissileSpeed = missileSpeed != 0.0f ? missileSpeed : 1.0f;
    }
    level.ambient = json.value("ambient", 1.0f);
    level.lightDirection =
        readVec3(json.value("lightDirection", nlohmann::json{}), level.lightDirection);
    level.lightColor = readVec3(json.value("lightColor", nlohmann::json{}), level.lightColor);
    level.lightIntensity = json.value("lightIntensity", 1.0f);
    return level;
}

LevelCameraInfo parseCamera(const nlohmann::json& json) {
    LevelCameraInfo camera;
    camera.minPitch = json.value("minPitch", 0.0f);
    camera.maxPitch = json.value("maxPitch", 0.0f);
    camera.boundsMin = readVec3(json.value("boundsMin", nlohmann::json{}), camera.boundsMin);
    camera.boundsMax = readVec3(json.value("boundsMax", nlohmann::json{}), camera.boundsMax);
    camera.attention = json.value("attention", 0.0f);
    camera.radiusMin = json.value("radiusMin", 0.0f);
    camera.radiusMax = json.value("radiusMax", 0.0f);
    camera.smooth = json.value("smooth", 0.0f);
    camera.minYaw = json.value("minYaw", 0.0f);
    camera.maxYaw = json.value("maxYaw", 0.0f);
    return camera;
}

BossCameraInfo parseBossCamera(const nlohmann::json& json) {
    BossCameraInfo camera;
    camera.flags = json.value("flags", 0U);
    camera.maxYaw = json.value("maxYaw", camera.maxYaw);
    camera.cosMaxYaw = json.value("cosMaxYaw", camera.cosMaxYaw);
    camera.minDistance = json.value("minDistance", camera.minDistance);
    camera.minPlayerDistance = json.value("minPlayerDistance", camera.minPlayerDistance);
    camera.maxDistance = json.value("maxDistance", camera.maxDistance);
    camera.maxPlayerDistance = json.value("maxPlayerDistance", camera.maxPlayerDistance);
    camera.minPitch = json.value("minPitch", camera.minPitch);
    camera.maxPitch = json.value("maxPitch", camera.maxPitch);
    camera.minAttention =
        readVec3(json.value("minAttention", nlohmann::json{}), camera.minAttention);
    camera.maxAttention =
        readVec3(json.value("maxAttention", nlohmann::json{}), camera.maxAttention);
    camera.keyAttention =
        readVec3(json.value("keyAttention", nlohmann::json{}), camera.keyAttention);
    camera.wizardAttention =
        readVec3(json.value("wizardAttention", nlohmann::json{}), camera.wizardAttention);
    return camera;
}

LevelAudioInfo parseAudio(const nlohmann::json& json) {
    LevelAudioInfo audio;
    audio.bank = json.value("bank", std::string{});
    audio.stream = json.value("stream", std::string{});
    audio.enterSound = json.value("enterSound", -1);
    audio.hitSound = json.value("hitSound", -1);
    audio.nameSound = json.value("nameSound", -1);
    return audio;
}

} // namespace

bool WorldData::load(const std::filesystem::path& file) {
    m_levels.clear();
    m_cameras.clear();
    m_audio.clear();
    m_sounds.clear();
    try {
        const std::vector<std::uint8_t> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        m_realm = root.value("realm", 0U);
        m_prefix = root.value("prefix", std::string{});
        std::vector<LevelEnemy> roster;
        for (const nlohmann::json& enemy : root.value("enemies", nlohmann::json::array())) {
            roster.push_back(LevelEnemy{enemy.value("kind", -1), enemy.value("subtype", 0)});
        }
        std::vector<BossCameraInfo> bossCameras;
        for (const nlohmann::json& camera : root.value("bossCameras", nlohmann::json::array())) {
            bossCameras.push_back(parseBossCamera(camera));
        }
        for (const nlohmann::json& level : root.at("levels")) {
            m_levels.push_back(parseLevel(level, roster));
            const std::int32_t bossCamera = level.value("bossCameraIndex", -1);
            if (bossCamera >= 0 && static_cast<std::size_t>(bossCamera) < bossCameras.size()) {
                m_levels.back().bossCamera = bossCameras[static_cast<std::size_t>(bossCamera)];
            }
        }
        for (const nlohmann::json& camera : root.value("cameras", nlohmann::json::array())) {
            m_cameras.push_back(parseCamera(camera));
        }
        for (const nlohmann::json& sound : root.value("sounds", nlohmann::json::array())) {
            m_sounds.push_back(sound.value("name", std::string{}));
        }
        for (const nlohmann::json& audio : root.value("audio", nlohmann::json::array())) {
            m_audio.push_back(parseAudio(audio));
        }
    } catch (const std::exception& e) {
        log::warn("World data {}: {}", file.string(), e.what());
        m_levels.clear();
        return false;
    }
    return !m_levels.empty();
}

const LevelInfo* WorldData::level(std::string_view name) const {
    for (const LevelInfo& level : m_levels) {
        if (level.name == name) {
            return &level;
        }
    }
    return nullptr;
}

const LevelCameraInfo* WorldData::camera(std::int32_t index) const {
    return index >= 0 && static_cast<std::size_t>(index) < m_cameras.size()
               ? &m_cameras[static_cast<std::size_t>(index)]
               : nullptr;
}

const LevelAudioInfo* WorldData::audio(std::int32_t index) const {
    return index >= 0 && static_cast<std::size_t>(index) < m_audio.size()
               ? &m_audio[static_cast<std::size_t>(index)]
               : nullptr;
}

std::string_view WorldData::soundName(std::int32_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= m_sounds.size()) {
        return {};
    }
    return m_sounds[static_cast<std::size_t>(index)];
}

float LevelTuning::experienceScale(std::int32_t level) const {
    const auto reached = static_cast<float>(level);
    if (playerLevel > 0.0f && reached > playerLevel) {
        return experience / (0.1f * (reached - playerLevel) + 1.0f);
    }
    return experience;
}

float LevelTuning::trapTimeScale(float gain) const {
    const float rate = trapRate * gain;
    return rate > 0.0f ? 1.0f / rate : 1.0f;
}

} // namespace gdl
