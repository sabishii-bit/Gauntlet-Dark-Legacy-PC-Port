#include "expansions/n64/audio/LegendsAmbient.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
} // namespace

bool LegendsAmbient::reset(std::span<const formats::LegendsN64Ambient> definitions) {
    m_emitters.clear();
    if (definitions.size() > 4096) {
        return false;
    }
    std::set<u32> identities;
    for (const auto& definition : definitions) {
        if (!finite(definition.position) || !std::isfinite(definition.radius) ||
            definition.radius <= 0 || definition.radius > 32767 ||
            !identities.insert(definition.nativeIndex).second) {
            return false;
        }
    }
    m_emitters.reserve(definitions.size());
    for (const auto& definition : definitions) {
        m_emitters.push_back({definition});
    }
    return true;
}

std::vector<LegendsAmbientVoice> LegendsAmbient::update(const Vec3& cameraTarget, bool departing) {
    std::vector<LegendsAmbientVoice> result;
    for (auto& emitter : m_emitters) {
        const auto& definition = emitter.definition;
        // e0038d04 computes three-dimensional distance to e0129eb8, the
        // camera's attention point. e007e184 stops all emitters on departure.
        const f32 distance = glm::distance(cameraTarget, definition.position);
        if (departing || !finite(cameraTarget) || !std::isfinite(distance) ||
            distance > definition.radius || definition.token == 0xffff) {
            emitter.active = false;
            continue;
        }
        if (!emitter.active) {
            emitter.active = true;
            emitter.previousGain = emitter.volume = 1;
        }
        const f32 desired = std::clamp(1 - distance / definition.radius, 0.0f, 1.0f);
        const f32 difference = desired - emitter.previousGain;
        // Native e007e184 uses a squared difference threshold of .02, not a
        // .02 linear deadband. Retain its previous unquantized comparison gain.
        if (difference * difference > .02f) {
            const auto byteVolume = static_cast<u32>(desired * 255);
            const auto nativeVolume = static_cast<u32>(static_cast<f32>(byteVolume) / 255 * 127);
            emitter.volume = static_cast<f32>(nativeVolume) / 127;
            emitter.previousGain = desired;
        }
        result.push_back({definition.nativeIndex, definition.token, definition.position,
                          definition.radius, emitter.volume});
    }
    return result;
}

} // namespace gdl::game
