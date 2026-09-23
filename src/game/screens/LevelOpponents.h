#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "engine/core/Types.h"

#include "game/enemies/Bosses.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/screens/BossMeter.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/CombatantProjectiles.h"
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
        f32 difficultyGain = 1;
    };
    struct Events {
        std::function<void(usize, f32, HurtKind, bool, const PlayerImpact&)> hurt;
        std::function<void(const Vec3&, f32, f32)> blast;
        std::function<void()> settleBlasts;
        std::function<void(const LegendEvent&)> legend;
        std::function<void(f32)> advanceLegend;
        std::function<void(const Vec3&)> fallen;
        std::function<void(const CombatSpew&)> spew;
        std::function<void(s32, f32)> advanceVictory;
        std::function<void()> levels;
        std::function<void(s32, s32, bool)> award;
        std::function<bool(const Vec3&, const Vec3&)> blocksBreath;
        std::function<bool(const Vec3&, const Vec3&)> blocksArea;
        std::function<std::vector<Mat4>()> arenaAnchors;
        std::function<std::vector<CombatArenaTarget>()> arenaTargets;
        std::function<void(const CombatArenaActivation&)> activateArena;
        std::function<void()> shake;
    };
    void open(const Resources& resources, std::span<const PlayerRuntime> players);
    void close();
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                std::span<const Obstacle> fixtures, const Events& events);
    static std::vector<EnemyView> enemyViews(std::span<const PlayerRuntime> players);
    /** Routes a contact by player identity; breath uses a shared quarter-second gate. */
    static void applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                 const Events& events);
    static void applyGrab(const CombatGrab& grab, bool boss, std::span<PlayerRuntime> players);
    void strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                     std::span<const PlayerRuntime> players);
    void strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                       std::optional<Vec3> where, bool close,
                       std::span<const PlayerRuntime> players);
    void strikeGenerator(s32 id, f32 power, s32 byPlayer);
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
    void showCritterCue(const CombatCue& cue, ItemArchive* archive, bool ofBoss);
    void followCritterEffects();
    std::optional<Resources> m_resources;
    Enemies m_enemies;
    Generators m_generators;
    Critters m_critters;
    Bosses m_bosses;
    BossMeter m_bossMeter;
    EnemyMissiles m_enemyMissiles;
    CombatantProjectiles m_combatantProjectiles;
    /** An effect riding on one of the great ones. */
    struct CritterEffect {
        u32 effect = 0;
        s32 critter = -1;
        bool ofBoss = false;
        Vec3 offset{0.0f, 0.0f, 0.0f}; ///< from the body
        std::optional<std::string> node;
        Vec3 nodeOffset{0.0f};
        bool rootAttachment = false;
        Vec2 pitchYaw{0.0f};
    };
    std::vector<CritterEffect> m_critterEffects;
    struct MoveEffect {
        u32 effect = 0;
        s32 critter = -1;
        bool ofBoss = false;
    };
    std::vector<MoveEffect> m_moveEffects;
    std::vector<u32>
        m_cueEffects; ///< all emitted cues, including detached effects borrowing artwork

    std::array<f32, 4> m_critterExperienceOwed{};
};
} // namespace gdl::game
