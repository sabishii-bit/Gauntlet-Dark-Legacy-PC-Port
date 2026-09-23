#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** One of the level's sound items: a loop at a spot, heard within its radius. */
struct AmbientEmitter {
    std::int32_t instance = -1;
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    SoundSet* bank = nullptr;
    std::uint32_t sound = 0;
    SoundHandle handle = kNoSound;
    float loudness = 0.0f; ///< 0 to 1, as last heard
};

/** Whose ears the loops are placed for: the camera, its right hand for the pan. */
struct AmbientEar {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
};

/**
 * The level's sound items, run the way the original runs them: each is a looping sound of
 * the level's banks named by its instance, at full loudness to any player within its radius,
 * fading straight to silence half a radius further out, panned by where it stands from the
 * camera, and stopped while nobody is near. The tower's are the realms' ambience at their
 * portals (drums, chains, bells, an organ, lava, wind) and the fire of every brazier.
 */
class AmbientSounds {
public:
    static constexpr std::int32_t kSoundItem = 13;
    static constexpr float kPeak = 224.0f / 255.0f; ///< the original's loudest
    static constexpr float kSilentAt = 1.5f;        ///< radii out where a loop has faded away

    /** How loud a loop of `radius` is `distance` away: 1 within, 0 past kSilentAt radii. */
    static float loudness(float distance, float radius);
    /** Where a spot sits between the ear's speakers, -1 left to 1 right. */
    static float panOf(const Vec3& position, const AmbientEar& ear);

    /** Takes every sound item whose name one of `banks` holds; false when there is none. */
    bool bind(const WorldLayout& layout, std::span<SoundSet* const> banks);
    /** Starts, adjusts and stops the loops for the listeners, at the level's sound volume. */
    void update(SoundPlayer& player, std::span<const Vec3> listeners, const AmbientEar& ear,
                float levelVolume);
    void stop(SoundPlayer& player);
    void clear();

    std::size_t size() const { return m_emitters.size(); }
    const AmbientEmitter& emitter(std::size_t index) const { return m_emitters[index]; }
    std::size_t playingCount() const;

private:
    std::vector<AmbientEmitter> m_emitters;
};

} // namespace gdl::game
