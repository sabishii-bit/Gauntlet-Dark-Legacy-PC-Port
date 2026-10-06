#include "game/world/AmbientSounds.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"
#include "game/world/MusicAreas.h"

namespace gdl::game {

f32 AmbientSounds::loudness(f32 distance, f32 radius) {
    // world_update, type 13: lbl_80346EF0 is 2.0, not zero. Small-radius
    // sound items bypass distance attenuation (music zones do not).
    if (radius <= 2.0f || distance <= radius) {
        return 1.0f;
    }
    // Linear from full at the radius to nothing at kSilentAt radii, as the original tapers.
    return std::clamp((kSilentAt * radius - distance) / ((kSilentAt - 1.0f) * radius), 0.0f, 1.0f);
}

f32 AmbientSounds::panOf(const Vec3& position, const AmbientEar& ear) {
    Vec3 away = position - ear.position;
    away.y = 0.0f;
    const f32 length = glm::length(away);
    if (length <= 0.0f) {
        return 0.0f;
    }
    Vec3 right = ear.right;
    right.y = 0.0f;
    const f32 span = glm::length(right);
    if (span <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(glm::dot(away / length, right / span), -1.0f, 1.0f);
}

bool AmbientSounds::bind(const WorldLayout& layout, std::span<SoundSet* const> banks,
                         const WorldScene* world) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != kSoundItem) {
            continue;
        }
        // A zone naming a music area plays no loop of its own (items.c 4514-4522).
        if (MusicAreas::areaOf(instance) > 0) {
            continue;
        }
        AmbientEmitter emitter;
        emitter.instance = static_cast<s32>(i);
        emitter.minPlayers = instance.minPlayers;
        emitter.position = instance.position;
        // SetItem calls FindWorldAnimNode after the initial world pose is applied.
        // Bind once from that pose, then keep following that node as it moves.
        f32 nearest = 10.0f;
        for (const auto& animation : layout.animations()) {
            if (animation.object < 0 ||
                static_cast<usize>(animation.object) >= layout.objects().size()) {
                continue;
            }
            const auto object = static_cast<usize>(animation.object);
            const Vec3 position = world != nullptr ? Vec3{world->worldTransform(object)[3]}
                                                   : layout.worldPosition(object);
            const f32 distance = glm::distance(instance.position, position);
            if (distance < nearest) {
                nearest = distance;
                emitter.parent = animation.object;
            }
        }
        // The radius leads the parameters as a float.
        std::memcpy(&emitter.radius, instance.params.data(), sizeof(emitter.radius));
        std::memcpy(&emitter.flags, &instance.params[8], sizeof(emitter.flags));
        for (SoundSet* bank : banks) {
            if (bank == nullptr) {
                continue;
            }
            if (const auto found = bank->find(instance.name); found.has_value()) {
                emitter.bank = bank;
                emitter.sound = *found;
                break;
            }
        }
        if (emitter.bank == nullptr) {
            log::warn("Ambient sounds: no bank holds {}", instance.name);
            continue;
        }
        m_emitters.push_back(emitter);
    }
    return !m_emitters.empty();
}

void AmbientSounds::update(SoundPlayer& player, std::span<const Vec3> listeners,
                           const AmbientEar& ear, f32 levelVolume,
                           std::optional<f32> volumeOverride, const WorldScene* world) {
    for (AmbientEmitter& emitter : m_emitters) {
        if (world != nullptr && emitter.parent >= 0) {
            emitter.position = Vec3{world->worldTransform(static_cast<usize>(emitter.parent))[3]};
        }
        f32 nearest = -1.0f;
        for (const Vec3& listener : listeners) {
            const f32 distance = glm::distance(listener, emitter.position);
            nearest = nearest < 0.0f ? distance : std::min(nearest, distance);
        }
        emitter.loudness = nearest < 0.0f || !shownToParty(emitter.minPlayers, m_players)
                               ? 0.0f
                               : loudness(nearest, emitter.radius);
        if (emitter.loudness <= 0.0f) {
            if (emitter.handle != kNoSound) {
                player.stop(emitter.handle);
                emitter.handle = kNoSound;
            }
            continue;
        }
        const f32 volume =
            std::clamp(volumeOverride.value_or(kPeak * emitter.loudness * levelVolume), 0.0f, 1.0f);
        if (emitter.handle == kNoSound || !player.isPlaying(emitter.handle)) {
            try {
                emitter.handle = player.play(emitter.bank->sequence(emitter.sound), volume,
                                             SoundCategory::Effects);
            } catch (const std::exception& e) {
                log::warn("Ambient sounds: {}: {}", emitter.bank->entry(emitter.sound).name,
                          e.what());
                emitter.handle = kNoSound;
                emitter.loudness = 0.0f;
                continue;
            }
        } else {
            player.setVolume(emitter.handle, volume);
        }
        player.setPan(emitter.handle, panOf(emitter.position, ear));
    }
}

void AmbientSounds::stop(SoundPlayer& player) {
    for (AmbientEmitter& emitter : m_emitters) {
        if (emitter.handle != kNoSound) {
            player.stop(emitter.handle);
            emitter.handle = kNoSound;
        }
        emitter.loudness = 0.0f;
    }
}

void AmbientSounds::clear() {
    m_emitters.clear();
    m_players = 1;
}

usize AmbientSounds::playingCount() const {
    return static_cast<usize>(std::ranges::count_if(
        m_emitters, [](const AmbientEmitter& emitter) { return emitter.handle != kNoSound; }));
}

std::optional<f32> AmbientSounds::musicScale() const {
    std::optional<f32> scale;
    for (const AmbientEmitter& emitter : m_emitters) {
        if ((emitter.flags & kDuckMusic) != 0 && emitter.loudness > 0.0f) {
            // AudioSecretProc, sounds.c 965: each audible flagged item overwrites the
            // request, in authored order. The normal proximity range gives 0.5..1.
            scale = 1.0f - 0.5f * emitter.loudness;
        }
    }
    return scale;
}

} // namespace gdl::game
