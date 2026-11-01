#pragma once

#include <functional>
#include <span>
#include <vector>

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/PlacedEnemies.h"
#include "game/mechanics/SwarmRules.h"

namespace gdl::game {

/** The native generator, swarm and missile phase, without player damage,
 * rewards or sounds. Its optional inter-phase callback owns effect dispatch.
 * Resource owners and world collision must outlive this borrowed view. */
class OpponentSimulation {
public:
    static constexpr s32 kCritterIds = 3000;
    static constexpr s32 kBossIds = 4000;

    OpponentSimulation(Generators& generators, Enemies& enemies, EnemyMissiles& missiles,
                       PlacedEnemies* placements = nullptr)
        : m_generators(generators), m_enemies(enemies), m_missiles(missiles),
          m_placements(placements) {}

    struct Frame {
        s32 ticks = 1;
        f32 seconds = 1.0f / 60;
        std::span<const EnemyView> players;
        std::span<const Obstacle> fixtures;
        std::span<const MissileStop> stops;
        std::span<const MissileTarget> critters;
        std::span<const MissileTarget> bosses;
        const WorldCollision* collision = nullptr;
        f32 missileSpeed = 1;
        bool timeStopped = false;
        SwarmTuning tuning; ///< how the mechanics tune the swarm this step
    };

    /** Call after native player/fixture motion, before processing opponent
     * outputs. The caller supplies current views, scales and combatant bodies.
     * Generator obstacles are sampled again after births, just as in native play. */
    /** Returns the exact stop roster used by this phase; the native combatant
     * projectile phase adjusts its rock health using the swarm's queued hits. */
    std::vector<MissileStop> advance(const Frame& frame,
                                     const EnemyMissiles::SceneryBlast& scenery = {},
                                     const std::function<void()>& afterEnemies = {});

    static std::vector<MissileTarget> targets(const Enemies& enemies,
                                              std::span<const MissileTarget> critters = {},
                                              std::span<const MissileTarget> bosses = {});

private:
    Generators& m_generators;
    Enemies& m_enemies;
    EnemyMissiles& m_missiles;
    PlacedEnemies* m_placements = nullptr;
};

} // namespace gdl::game
