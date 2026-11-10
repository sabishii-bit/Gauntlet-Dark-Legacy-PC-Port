#include "expansions/dreamcast/audio/LegendsDreamcastAmbient.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

#include "game/audio/AmbientSounds.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

u16 shortAt(const std::array<u8, 12>& params, usize offset) {
    return static_cast<u16>(params[offset] | (static_cast<u16>(params[offset + 1]) << 8U));
}

f32 radiusOf(const std::array<u8, 12>& params) {
    const u32 bits = static_cast<u32>(params[0]) | (static_cast<u32>(params[1]) << 8U) |
                     (static_cast<u32>(params[2]) << 16U) | (static_cast<u32>(params[3]) << 24U);
    return std::bit_cast<f32>(bits);
}
} // namespace

bool LegendsDreamcastAmbient::reset(const formats::LegendsDreamcastScene& scene, s32 partyCount,
                                    const TokenResolver& resolve) {
    m_emitters.clear();
    if (!resolve || partyCount < 1 || partyCount > 4) {
        return false;
    }
    for (usize index = 0; index < scene.itemInstances.size(); ++index) {
        const auto& instance = scene.itemInstances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= scene.itemInfos.size() ||
            scene.itemInfos[static_cast<usize>(instance.info)].type != 13 ||
            !shownToParty(instance.minPlayers, partyCount)) {
            continue;
        }
        // SetItem 8c0a8f20..8c0a9040 resolves a name only for mode zero.
        // Positive modes are music-zone selectors, not audible sound tokens.
        if (shortAt(instance.params, 4) != 0 || instance.name.empty() ||
            !finite(instance.position)) {
            continue;
        }
        const f32 radius = radiusOf(instance.params);
        if (!std::isfinite(radius) || radius < 0) {
            continue;
        }
        std::string name = instance.name;
        for (char& c : name) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        const auto token = resolve(name);
        if (!token) {
            continue;
        }
        m_emitters.push_back({static_cast<u32>(index), *token, instance.position, radius,
                              shortAt(instance.params, 8)});
    }
    return !m_emitters.empty();
}

std::optional<LegendsDreamcastAmbient::Request>
LegendsDreamcastAmbient::request(const Vec3& attention, const PositionResolver& positions) const {
    if (!finite(attention)) {
        return {};
    }
    std::optional<Request> result;
    f32 nearest = std::numeric_limits<f32>::infinity();
    for (const auto& emitter : m_emitters) {
        const Vec3 position = positions ? positions(emitter.nativeIndex).value_or(emitter.position)
                                        : emitter.position;
        if (!finite(position)) {
            continue;
        }
        const f32 distance = glm::distance(attention, position);
        const f32 loudness = AmbientSounds::loudness(distance, emitter.radius);
        if (!std::isfinite(distance) || loudness <= 0 || distance >= nearest) {
            continue;
        }
        // do_items 8c0ace62..8c0acede keeps only the closest audible item.
        // AudioSecretProc 8c0dd380 truncates its 224-step gain, restarts a
        // finished native sample while selected, and stops the previous source.
        const f32 volume = static_cast<f32>(static_cast<s32>(224.f * loudness)) / 255.f;
        if (volume <= 0) {
            continue;
        }
        Request selected{
            emitter.nativeIndex, emitter.token, position, loudness, volume, emitter.flags, {}};
        if ((emitter.flags & 1U) != 0) {
            // Unlike GameCube's half-strength duck, DC passes 1-gain to
            // AudioMusicDamp 8c0dd100, which clamps it to [0.2, 1].
            selected.musicScale = std::clamp(1.f - loudness, .2f, 1.f);
        }
        nearest = distance;
        result = selected;
    }
    return result;
}

} // namespace gdl::game
