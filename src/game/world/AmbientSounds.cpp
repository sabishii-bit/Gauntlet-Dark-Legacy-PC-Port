#include "game/world/AmbientSounds.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>

#include "engine/core/Log.h"

namespace gdl::game {

float AmbientSounds::loudness(float distance, float radius) {
    if (radius <= 0.0f || distance <= radius) {
        return 1.0f;
    }
    // Linear from full at the radius to nothing at kSilentAt radii, as the original tapers.
    return std::clamp((kSilentAt * radius - distance) / ((kSilentAt - 1.0f) * radius), 0.0f, 1.0f);
}

float AmbientSounds::panOf(const Vec3& position, const AmbientEar& ear) {
    Vec3 away = position - ear.position;
    away.y = 0.0f;
    const float length = glm::length(away);
    if (length <= 0.0f) {
        return 0.0f;
    }
    Vec3 right = ear.right;
    right.y = 0.0f;
    const float span = glm::length(right);
    if (span <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(glm::dot(away / length, right / span), -1.0f, 1.0f);
}

bool AmbientSounds::bind(const WorldLayout& layout, std::span<SoundSet* const> banks) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= infos.size() ||
            infos[static_cast<std::size_t>(instance.info)].type != kSoundItem) {
            continue;
        }
        AmbientEmitter emitter;
        emitter.instance = static_cast<int>(i);
        emitter.position = instance.position;
        // The radius leads the parameters as a float.
        std::memcpy(&emitter.radius, instance.params.data(), sizeof(emitter.radius));
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
                           const AmbientEar& ear, float levelVolume) {
    for (AmbientEmitter& emitter : m_emitters) {
        float nearest = -1.0f;
        for (const Vec3& listener : listeners) {
            const float distance = glm::distance(listener, emitter.position);
            nearest = nearest < 0.0f ? distance : std::min(nearest, distance);
        }
        emitter.loudness = nearest < 0.0f ? 0.0f : loudness(nearest, emitter.radius);
        if (emitter.loudness <= 0.0f) {
            if (emitter.handle != kNoSound) {
                player.stop(emitter.handle);
                emitter.handle = kNoSound;
            }
            continue;
        }
        const float volume = std::clamp(kPeak * emitter.loudness * levelVolume, 0.0f, 1.0f);
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
}

std::size_t AmbientSounds::playingCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        m_emitters, [](const AmbientEmitter& emitter) { return emitter.handle != kNoSound; }));
}

} // namespace gdl::game
