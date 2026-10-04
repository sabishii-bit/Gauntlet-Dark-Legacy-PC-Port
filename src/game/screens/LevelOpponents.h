#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "engine/core/Types.h"

#include "game/enemies/Bosses.h"
#include "game/enemies/CritterStatues.h"
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
        /** Whether the level's placed enemies wait to be seen (watch) before they stand;
         * otherwise they all stand as it opens. */
        bool standOnSight = false;
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
        std::function<void(s32)> destroyedGenerator;
        std::function<bool(const Vec3&, const Vec3&)> blocksBreath;
        std::function<bool(const Vec3&, const Vec3&)> blocksArea;
        std::function<std::vector<Mat4>()> arenaAnchors;
        std::function<std::vector<CombatArenaTarget>()> arenaTargets;
        std::function<void(const CombatArenaActivation&)> activateArena;
        std::function<void()> shake;
        /** A lesson for the whole party, over the first standing player. */
        std::function<bool(s32, usize)> help;
        EnemyMissiles::SceneryBlast blastScenery;
    };
    void open(const Resources& resources, std::span<const PlayerRuntime> players);
    void close();
    /** What the camera takes in and where it looks: what is on screen, and the placed
     * enemies it comes to see stand (fn_80060114). */
    void watch(const ViewVolume& view, const Vec3& attention);
    usize pendingPlacements() const { return m_pending.size(); }
    /** `stops` are what else stands in the way of the swarm's missiles, `walkedInto` what
     * of the fixtures stands where the great ones walk. */
    /** Follows moving scenery without advancing AI, attacks or animation. */
    void syncFloors();
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                std::span<const Obstacle> fixtures, const Events& events,
                std::span<const MissileStop> stops = {},
                std::span<const CombatantObstacle> walkedInto = {});
    /** The great ones' blows on barrels they walked into since the last call (by index). */
    std::vector<CombatantRam> takeBarrelRams() { return std::exchange(m_barrelRams, {}); }
    /** Swarm and combatant projectile blows on safe rocks, for the level. */
    std::vector<RockHit> takeRockHits() { return std::exchange(m_rockHits, {}); }
    /** Where the swarm's blasts reached since the last call, for pickup damage. */
    std::vector<PickupBlastReach> takePickupBlasts() { return m_enemyMissiles.takePickupBlasts(); }
    /** Drain hits from the last projectile/attack phase before a level transition freezes
     * simulation. No AI, collision, or time advances, and each reward is consumed once. */
    void settleRewards(std::span<const PlayerRuntime> players, const Events& events);
    /** Releases a chest's Death record; count distinguishes red from black. */
    bool releaseDeath(s32 record, const Vec3& position, s32 count);
    /** Stops the contact-drain loop while simulation is paused. */
    void stopDeathSound();
    static std::vector<EnemyView> enemyViews(std::span<const PlayerRuntime> players);
    Vec3 resolveMovement(const PlayerActor& player, const Vec3& from, const Vec3& to) const;
    /** Routes a contact by player identity; breath uses a shared quarter-second gate. */
    static bool applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                 const Events& events);
    static void applyGrab(const CombatGrab& grab, bool boss, std::span<PlayerRuntime> players);
    /** Returns the swarm's healing credit, before level/armor adjustments. */
    f32 strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                    std::span<const PlayerRuntime> players, bool close = false,
                    std::optional<Vec3> where = std::nullopt);
    /** Returns healing credit after armor, but before level/hit-node adjustments. */
    f32 strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                      std::optional<Vec3> where, bool close, std::span<const PlayerRuntime> players,
                      s32 node = -1);
    void strikeGenerator(s32 id, f32 power, s32 byPlayer,
                         std::span<const PlayerRuntime> players = {});
    /** A blast of `damage` over `radius` reaching what it has not yet (`reached`, its ids:
     * enemies, generators from 1000, critters from 2000, the boss 3000). */
    void blast(const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
               std::span<const PlayerRuntime> players, u32 flags = EnemyHit::kKnockDown);
    /** What a character of `level` hits a generator by at a place meant for `placeLevel`: a
     * hundredth less a level under it, a tenth more a level over it. */
    static f32 generatorPowerScale(s32 level, f32 placeLevel);
    Enemies& enemies() { return m_enemies; }
    const Enemies& enemies() const { return m_enemies; }
    Generators& generators() { return m_generators; }
    const Generators& generators() const { return m_generators; }
    Critters& critters() { return m_critters; }
    const Critters& critters() const { return m_critters; }
    /** The golems, gargoyles and Deaths still standing as item statues. */
    const CritterStatues& statues() const { return m_statues; }
    /** A blow on a statue wakes it (fn_8005C1DC's placed-enemy case). */
    void wakeStatue(usize index) { m_statues.wake(index); }
    /** A trigger flagged to wake a statue went active at `spot`: the placed enemy nearest it
     * within ten, statue or not, is woken, which only a statue takes any notice of. */
    void wakeStatueNear(const Vec3& spot);
    Bosses& bosses() { return m_bosses; }
    const Bosses& bosses() const { return m_bosses; }
    const EnemyMissiles& missiles() const { return m_enemyMissiles; }
    BossMeters& meter() { return m_bossMeter; }
    const BossMeters& meter() const { return m_bossMeter; }
    /** The lights the swarm's lobs and their bursts give off. */
    void lights(std::vector<PointLight>& out) const;
    /** How loud a sound made at `at` is heard: full within twenty of the nearest standing
     * player, fading to nothing at seventy (sndFxPlay3DAtten). */
    static f32 attenuation(const Vec3& at, std::span<const Vec3> hearers);

private:
    /** Plays `name` at `level` of 255 as heard from where the players stand. */
    SoundHandle playAt(std::string_view name, f32 level, const Vec3& at);
    void landEnemyMissiles(std::span<PlayerRuntime> players, const Events& events);
    void playEnemyCues();
    std::vector<MissileTarget> swarmTargets() const;
    void strikeSwarm(s32 id, f32 damage, u32 flags, const Vec3& direction,
                     std::span<const PlayerRuntime> players);
    void explodeSuicide(const EnemyBurst& burst);
    void advanceClouds();
    /** A suicide's poison cloud, turning from one tree to the next. */
    struct Cloud {
        u32 effect = 0;
        usize stage = 0; ///< the tree to turn to next
        Vec3 position{0.0f};
    };
    std::vector<Cloud> m_clouds;
    /** The garm brood's death shot: its burst, then the shot that flies on from it. */
    struct DeathShot {
        u32 burst = 0; ///< the effect held where the corpse lay; nought once it is over
        s32 kind = kGarmBroodKind;
        Vec3 position{0.0f};
        Vec3 velocity{0.0f};
        bool flying = false; ///< the shot's own effect has been sent on its way
    };
    std::vector<DeathShot> m_deathShots;
    void fireDeathShot(const EnemyDeathShot& shot);
    void advanceDeathShots();
    void hearFrom(std::span<const PlayerRuntime> players);
    void applyEnemyBlow(const EnemyBlow& blow, std::span<PlayerRuntime> players,
                        const Events& events);
    void awardEnemyLosses(const Events& events);
    void awardBossLosses(std::span<const PlayerRuntime> players, const Events& events);
    void awardCritterLosses(std::span<const PlayerRuntime> players, const Events& events);
    void showCritterCue(const CombatCue& cue, ItemArchive* archive, bool ofBoss);
    void followCritterEffects(std::span<const PlayerRuntime> players);
    void updateDeaths(std::span<PlayerRuntime> players, const Events& events);
    void clearDeaths();
    void finishSummons(std::span<const EnemyView> players);
    /** A placed enemy or great one waiting to be seen. */
    struct Placement {
        EnemySpawn spawn;
        s32 kind = -1;
        f32 facing = 0.0f;
        f32 viewRadius = 0.0f;
        f32 sight = 0.0f;             ///< the placement's sight radius, a great one's visrad
        std::optional<usize> carried; ///< the pickup a great one holds, as the level opens
    };
    void standPlacements(std::optional<ViewVolume> view, const Vec3& attention);
    void stand(const Placement& placement);
    /** Whether the camera sees a sphere from close enough for a placement to stand. */
    bool inView(const Vec3& at, f32 radius) const;
    /** Stands the statues woken and seen whose ACTIVE has played out as the great ones they
     * are, and wakes what the triggers ask. */
    void updateStatues(s32 ticks, f32 seconds, std::span<PlayerRuntime> players);
    std::vector<Placement> m_pending;
    CritterStatues m_statues;
    std::optional<ViewVolume> m_view; ///< what the camera last took in, and from where
    Vec3 m_attention{0.0f};
    /** What each of the great ones carries, by its place in the pool. */
    std::array<std::optional<usize>, Critters::kMost> m_carried{};
    struct Bag {
        std::optional<usize> item;
        std::string name;
        Vec3 position{0};
        f32 velocity = 20;
        f32 age = 0;
        f32 pitch = 0;
        f32 spin = 0;
        u32 effect = 0;
        bool landed = false;
    };
    std::vector<Bag> m_bags;
    u32 m_bagSeed = 1;
    void updateBags(f32 seconds);
    void releaseBag(const Bag& bag);
    /** A great one slain lets what it carried go, a gargoyle its piece of the wings if it
     * carried nothing (CritterDropItem), with the lesson its kind teaches. */
    void dropCarried(const CombatLoss& loss, std::span<const PlayerRuntime> players,
                     const Events& events);
    std::optional<Resources> m_resources;
    Enemies m_enemies;
    Generators m_generators;
    Critters m_critters;
    std::vector<CombatantRam> m_barrelRams;
    std::vector<RockHit> m_rockHits;
    /** Each player's hits on the swarm a generator bred since they last destroyed one (the
     * original's hit_streak), by player id. */
    static constexpr usize kPlayerIds = 4; ///< players are numbered 0 to 3
    std::array<s32, kPlayerIds> m_hitStreak{};
    /** Lessons waiting for the next settling: which, and for which player id. */
    std::vector<std::pair<s32, s32>> m_lessons;
    static constexpr s32 kStreakLesson = 10; ///< this many hits teach to destroy generators
    static constexpr s32 kGeneratorRamBase = 2000;
    static constexpr std::string_view kHitFlashSkin = "AAAWHITE"; ///< of POWERUPS
    static constexpr f32 kShortGenerator = 3.0f; ///< a great one breaks one no taller
    std::vector<CombatantObstacle>
    critterObstacles(std::span<const CombatantObstacle> fixtures) const;
    Bosses m_bosses;
    BossMeters m_bossMeter;
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
        std::optional<s32> playerAttachment;
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
    std::array<u32, Enemies::kMost> m_deathEffects{};
    SoundHandle m_deathSound = kNoSound;
    bool m_deathContact = false;
    std::vector<SoundHandle> m_yells; ///< suicides' cries, cut short when one is struck down
    std::vector<Vec3> m_hearers;      ///< where the standing players are, for how loud
    /** The scale the party's enemy shrinkers held the swarm at last update, for the sound
     * of it rising back (SetPlayerVars). */
    f32 m_shrink = 1.0f;
    /** Holds the swarm, the great ones and their missiles at the shrinkers' scale. */
    void shrinkOpponents(std::span<const PlayerRuntime> players);

    std::array<f32, 4> m_critterExperienceOwed{};
    /** What players' blows on generators earned: who, how much, whether destroyed; drained
     * on the next settling. */
    struct GeneratorReward {
        s32 player = -1;
        s32 experience = 0;
        bool destroyed = false;
    };
    std::vector<GeneratorReward> m_generatorRewards;
};
} // namespace gdl::game
