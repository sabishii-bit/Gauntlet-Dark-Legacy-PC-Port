#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "game/enemies/Bosses.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/screens/BossMeter.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/CritterProjectiles.h"
#include "game/world/EffectTrees.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"
namespace gdl::game {
/** Level enemy population, damage/reward routing and following combat cues.
 * Owns enemy archives, missiles, generators and the boss meter. Borrowed level services
 * must outlive close(). Phase hooks synchronously interleave scene-level rites,
 * victories and player progression; no party spans or callbacks are retained. */
class LevelOpponents {
public:
    struct Resources {
        RenderDevice& device;
        LevelWorld& world;
        ItemArchive& weapons;
        EffectTrees& effects;
        LevelSoundscape& audio;
        std::filesystem::path root;
        float difficultyGain = 1;
    };
    struct Events {
        std::function<void(std::size_t, float, HurtKind, bool)> hurt;
        std::function<void(const Vec3&, float, float)> blast;
        std::function<void()> settleBlasts;
        std::function<void(const LegendEvent&)> legend;
        std::function<void(float)> advanceLegend;
        std::function<void(const Vec3&)> fallen;
        std::function<void(const CritterSpew&)> spew;
        std::function<void(std::int32_t, float)> advanceVictory;
        std::function<void()> levels;
        std::function<void(std::int32_t, std::int32_t, bool)> award;
    };
    void open(const Resources& resources, std::span<const PlayerRuntime> players);
    void close();
    void update(std::int32_t ticks, float seconds, std::span<PlayerRuntime> players,
                std::span<const Obstacle> fixtures, const Events& events);
    static std::vector<EnemyView> enemyViews(std::span<const PlayerRuntime> players);
    /** Routes a contact by player identity; breath uses a shared quarter-second gate. */
    static void applyCritterBlow(const CritterBlow& blow, std::span<PlayerRuntime> players,
                                 const Events& events);
    void strikeEnemy(std::int32_t id, float power, std::uint32_t flags, const Vec3& direction,
                     std::int32_t byPlayer, std::span<const PlayerRuntime> players);
    void strikeCritter(std::int32_t id, float power, std::uint32_t flags, const Vec3& direction,
                       std::int32_t byPlayer, std::optional<Vec3> where, bool close,
                       std::span<const PlayerRuntime> players);
    void strikeGenerator(std::int32_t id, float power, std::int32_t byPlayer);
    Enemies& enemies() { return m_enemies; }
    const Enemies& enemies() const { return m_enemies; }
    Generators& generators() { return m_generators; }
    const Generators& generators() const { return m_generators; }
    Critters& critters() { return m_critters; }
    const Critters& critters() const { return m_critters; }
    Bosses& bosses() { return m_bosses; }
    const Bosses& bosses() const { return m_bosses; }
    const EnemyMissiles& missiles() const { return m_enemyMissiles; }
    BossMeter& meter() { return m_bossMeter; }
    const BossMeter& meter() const { return m_bossMeter; }

private:
    void awardBossLosses(std::span<const PlayerRuntime> players, const Events& events);
    void awardCritterLosses(std::span<const PlayerRuntime> players, const Events& events);
    void showCritterCue(const CritterCue& cue, ItemArchive* archive, bool ofBoss);
    void followCritterEffects();
    std::optional<Resources> m_resources;
    Enemies m_enemies;
    Generators m_generators;
    Critters m_critters;
    Bosses m_bosses;
    BossMeter m_bossMeter;
    EnemyMissiles m_enemyMissiles;
    CritterProjectiles m_critterProjectiles;
    /** An effect riding on one of the great ones. */
    struct CritterEffect {
        std::uint32_t effect = 0;
        std::int32_t critter = -1;
        bool ofBoss = false;
        Vec3 offset{0.0f, 0.0f, 0.0f}; ///< from the body
        std::optional<std::string> node;
        Vec3 nodeOffset{0.0f};
    };
    std::vector<CritterEffect> m_critterEffects;

    std::array<float, 4> m_critterExperienceOwed{};
};
} // namespace gdl::game
