#include "engine/assets/WorldData.h"

#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

Vec3 readVec3(const nlohmann::json& array, const Vec3& fallback) {
    if (!array.is_array() || array.size() < 3) {
        return fallback;
    }
    return Vec3{array.at(0).get<f32>(), array.at(1).get<f32>(), array.at(2).get<f32>()};
}

LevelInfo parseLevel(const nlohmann::json& json) {
    LevelInfo level;
    level.name = json.value("name", std::string{});
    level.title = json.value("title", std::string{});
    level.audioBank = json.value("audioBank", std::string{});
    level.movie = json.value("movie", std::string{});
    level.cameraIndex = json.value("cameraIndex", -1);
    level.audioIndex = json.value("audioIndex", -1);
    level.musicVolume = json.value("musicVolume", 1.0f);
    level.soundVolume = json.value("soundVolume", 1.0f);
    if (const auto tuning = json.find("tuning"); tuning != json.end() && tuning->is_object()) {
        // A zero stands for the difficulty, itself one when it is zero.
        const f32 difficulty = tuning->value("difficulty", 0.0f);
        level.tuning.difficulty = difficulty != 0.0f ? difficulty : 1.0f;
        const auto scaled = [&](const char* key) {
            const f32 value = tuning->value(key, 0.0f);
            return value != 0.0f ? value : level.tuning.difficulty;
        };
        const f32 damage = tuning->value("damage", 0.0f);
        level.tuning.damage = damage != 0.0f ? damage : 1.0f;
        level.tuning.playerLevel = tuning->value("playerLevel", 0.0f);
        const f32 experience = tuning->value("experience", 0.0f);
        level.tuning.experience = experience != 0.0f ? experience : 1.0f;
        level.tuning.trapRate = scaled("trapRate");
        level.tuning.trapDamage = scaled("trapDamage");
    }
    level.ambient = json.value("ambient", 1.0f);
    level.lightDirection = readVec3(json.value("lightDirection", nlohmann::json{}),
                                    level.lightDirection);
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
        const std::vector<u8> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        m_realm = root.value("realm", 0U);
        m_prefix = root.value("prefix", std::string{});
        for (const nlohmann::json& level : root.at("levels")) {
            m_levels.push_back(parseLevel(level));
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

const LevelCameraInfo* WorldData::camera(s32 index) const {
    return index >= 0 && static_cast<usize>(index) < m_cameras.size()
               ? &m_cameras[static_cast<usize>(index)]
               : nullptr;
}

const LevelAudioInfo* WorldData::audio(s32 index) const {
    return index >= 0 && static_cast<usize>(index) < m_audio.size()
               ? &m_audio[static_cast<usize>(index)]
               : nullptr;
}

std::string_view WorldData::soundName(s32 index) const {
    if (index < 0 || static_cast<usize>(index) >= m_sounds.size()) {
        return {};
    }
    return m_sounds[static_cast<usize>(index)];
}

f32 LevelTuning::experienceScale(s32 level) const {
    const auto reached = static_cast<f32>(level);
    if (playerLevel > 0.0f && reached > playerLevel) {
        return experience / (0.1f * (reached - playerLevel) + 1.0f);
    }
    return experience;
}

f32 LevelTuning::trapTimeScale(f32 gain) const {
    const f32 rate = trapRate * gain;
    return rate > 0.0f ? 1.0f / rate : 1.0f;
}

} // namespace gdl
