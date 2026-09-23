#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/ParticleTemplateJson.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

Vec3 readVec3(const Json& array) {
    return Vec3{array.at(0).get<f32>(), array.at(1).get<f32>(), array.at(2).get<f32>()};
}

/** Four numbers of an array, or the defaults when the entry is missing. */
template <typename T>
std::array<T, 4> readFour(const Json& entry, const char* key, std::array<T, 4> fallback) {
    if (!entry.contains(key)) {
        return fallback;
    }
    const auto values = entry.at(key).get<std::vector<T>>();
    for (usize i = 0; i < fallback.size() && i < values.size(); ++i) {
        fallback[i] = values[i];
    }
    return fallback;
}

} // namespace

ParticleTemplate readParticleTemplate(const Json& entry) {
    ParticleTemplate particle;
    const auto id = entry.value("id", std::string{});
    particle.id = id.empty() ? '\0' : id[0];
    particle.preset = entry.value("preset", 0U);
    particle.flags = entry.value("flags", 0U);
    particle.flagMask = entry.value("flagMask", 0U);
    particle.enables = entry.value("enables", 0U);
    particle.maxParticles = entry.value("maxParticles", 0);
    particle.maxDirections = entry.value("maxDirections", 0U);
    particle.maxPositions = entry.value("maxPositions", 0U);
    const auto emitterLife = entry.value("emitterLife", std::vector<f32>{0.0f, 0.0f});
    const auto particleLife = entry.value("particleLife", std::vector<f32>{0.0f, 0.0f});
    for (usize i = 0; i < 2; ++i) {
        particle.emitterLife[i] = i < emitterLife.size() ? emitterLife[i] : 0.0f;
        particle.particleLife[i] = i < particleLife.size() ? particleLife[i] : 0.0f;
    }
    particle.angle = entry.value("angle", 0.0f);
    particle.textureCount = entry.value("textureCount", 0);
    particle.texture = normalizeAssetName(entry.value("texture", std::string{}));
    if (entry.contains("direction")) {
        particle.direction = readVec3(entry.at("direction"));
    }
    if (entry.contains("volume")) {
        particle.volume = readVec3(entry.at("volume"));
    }
    particle.rate = readFour<f32>(entry, "rate", particle.rate);
    particle.rateRandom = entry.value("rateRandom", 0.0f);
    particle.gravity = entry.value("gravity", 0.0f);
    particle.drag = entry.value("drag", 0.0f);
    particle.speed = entry.value("speed", 0.0f);
    particle.rgba = readFour<u32>(entry, "rgba", particle.rgba);
    particle.width = readFour<f32>(entry, "width", particle.width);
    particle.delay = entry.value("delay", 0.0f);
    return particle;
}

} // namespace gdl
