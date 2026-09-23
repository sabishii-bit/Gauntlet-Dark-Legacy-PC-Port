#pragma once

#include <cstddef>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

/** Someone a trap can hurt. */
struct TrapVictim {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.75f;
};

/** A trap caught someone this update. */
struct TrapHit {
    std::size_t trap = 0;
    std::size_t victim = 0;
    float damage = 0.0f;
    int subtype = 0;      ///< which picks the trap's sound
    bool pierces = false; ///< spikes and blades, which a victim groans at
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/**
 * A level's traps (spikes, flames, saw blades), cycled the way the original cycles them: each
 * rests on its first sequence for a time drawn afresh each round (its record's off time,
 * negative meaning at random between half of it and one and a half), then plays its others
 * one after another and rests again. While it is out of its rest whoever stands in its box
 * is hurt by its record's value, and is then left alone until the sequence it was caught in
 * has run out twice over, as the original leaves them. The level scales the times and the
 * damage.
 */
class Traps {
public:
    static constexpr int kResting = 0;
    static constexpr float kSecondsPerTickLeft = 1.0f / 30.0f; ///< a victim's respite
    static constexpr int kSpikes = 0;                          ///< the subtypes that pierce
    static constexpr int kBlade = 3;
    static constexpr int kBlades = 4;
    static constexpr int kTicksPerTimeUnit = 2; ///< the record's times are in half ticks

    /** One trap. */
    struct Trap {
        int instance = -1;
        float damage = 0.0f;
        int offTime = 0; ///< the record's, in its own units
        int subtype = 0;
        int action = kResting;
        int ticksLeft = 0;
        int minPlayers = 0;
        bool shown = true;
        ItemFigure figure;
        Obstacle box;
    };

    /** `timeScale` stretches every rest and `damageScale` every hurt, as the level says. */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const WorldCollision* collision, unsigned int seed = 1, float timeScale = 1.0f,
              float damageScale = 1.0f);
    void clear();
    std::size_t size() const { return m_traps.size(); }
    const Trap& trap(std::size_t index) const { return *m_traps[index]; }
    void setPlayerCount(int players);
    /** Whether a trap is out of its rest, and hurts. */
    bool armed(std::size_t index) const { return m_traps[index]->action != kResting; }

    std::vector<TrapHit> update(int ticks, float seconds, std::span<const TrapVictim> party);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

private:
    int restTicks(const Trap& trap);

    std::vector<std::unique_ptr<Trap>> m_traps;
    std::vector<float> m_gaps; ///< per victim, seconds before they can be hurt again
    std::minstd_rand m_random{1};
    float m_timeScale = 1.0f;
};

} // namespace gdl::game
