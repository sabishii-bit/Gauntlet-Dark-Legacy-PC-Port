#pragma once
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"

#include "expansions/shared/enemies/LegendsActors.h"
#include "game/audio/LevelSoundscape.h"
#include "game/bosses/Bosses.h"
#include "game/critters/CombatantProjectiles.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/BossEncounterHost.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/OpponentCues.h"
#include "game/enemies/OpponentHost.h"
#include "game/enemies/OpponentPool.h"
#include "game/enemies/PlacedEnemies.h"
#include "game/hud/BossMeter.h"
#include "game/level/CombatCover.h"
#include "game/level/LevelWorld.h"
#include "game/level/MissileStop.h"
#include "game/mechanics/MechanicsFacade.h"
#include "game/party/PlayerRuntime.h"
namespace gdl::game {
struct GeneratorFeedbackEmission;
class ImportedOpponentCues;
/** Level enemy population, damage/reward routing and following combat cues.
 * Owns enemy archives, missiles, generators and the boss meter. Borrowed level services
 * must outlive close(). Its hosts are answered synchronously at their phases, interleaving
 * scene-level rites, victories and player progression; no party spans or hosts are
 * retained. */
class LevelOpponents {
public:
    LevelOpponents() = default;
    ~LevelOpponents() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LevelOpponents);
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
    void open(const Resources& resources, std::span<const PlayerRuntime> players);
    void close();
    /** Current GDL damage state for the level-owned native generator figures.
     * Failed installations remain pool=-1 so their artwork is not falsely destroyed. */
    std::vector<ImportedGeneratorState> importedGeneratorStates() const;
    /** What the camera takes in and where it looks: what is on screen, and the placed
     * enemies it comes to see stand (fn_80060114). */
    /** What the swarm and the generators count as on screen; the placed enemies the view
     * comes to see stand as well, unless `standPlaced` is off (the entrance, when
     * fn_80060114 waits for the level's earlyenemies). */
    void watch(const ViewVolume& view, const Vec3& attention, bool standPlaced = true);
    /** The same for what the party's cameras take in together, under solo cameras. */
    void watch(const ViewSet& views, bool standPlaced = true);
    usize pendingPlacements() const { return m_placements.count(); }
    PlacedEnemies& placements() { return m_placements; }
    const PlacedEnemies& placements() const { return m_placements; }
    /** Original load-time tuning, unaffected by later changes to game options. */
    f32 difficultyGain() const { return m_resources ? m_resources->difficultyGain : 1.0f; }
    /** The additive mechanics, borrowed until close and handed on to the swarm and its
     * generators; the original's rules when null. */
    void setMechanics(MechanicsFacade* mechanics);
    /** The party size the generators breed for instead of the players present, or none. */
    void setPartyCount(std::optional<s32> count) { m_partyCountOverride = count; }
    /** The living swarm, for presentation drawn over it. */
    std::vector<SwarmBodyView> swarmBodies() const;
    /** Imported boss mesh effects share their parent's presented hierarchy;
     * ordinary levels retain their existing effect interpolation unchanged. */
    std::vector<EffectTrees::DrawAttachment> presentedBossEffects(f32 alpha) const;
    usize bagCount() const { return m_sim.bags.size(); }
    /** Recorder attachment preflight, never a gameplay decision or mixer poll.
     * Paused/silent legacy Death contact has no logical birth to carry across. */
    bool hasLegacyAudio() const {
        if (m_deathSound != kNoSound || m_resumeDeathSound ||
            (m_sim.deathContact && !m_sim.deathSound)) {
            return true;
        }
        return std::ranges::any_of(m_yells, [](SoundHandle yell) { return yell != kNoSound; });
    }
    /** Outside-pool phases that can change bodies, items, or missile blockers.
     * Ordinary native enemies and generators alone do not trigger this gate. */
    bool hasUncapturedState() const;
    /** `stops` are what else stands in the way of the swarm's missiles, `walkedInto` what
     * of the fixtures stands where the great ones walk. */
    /** Follows moving scenery without advancing AI, attacks or animation. */
    void syncFloors();
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                std::span<const Obstacle> fixtures, OpponentHost& host,
                BossEncounterHost& encounter, const CombatCover& cover,
                std::span<const MissileStop> stops = {},
                std::span<const CombatantObstacle> walkedInto = {});
    /** The great ones' blows on barrels they walked into since the last call (by index). */
    std::vector<CombatantRam> takeBarrelRams() { return std::exchange(m_sim.barrelRams, {}); }
    /** Swarm and combatant projectile blows on safe rocks, for the level. */
    std::vector<RockHit> takeRockHits() { return std::exchange(m_sim.rockHits, {}); }
    /** Where the swarm's blasts reached since the last call, for pickup damage. */
    std::vector<PickupBlastReach> takePickupBlasts() { return m_enemyMissiles.takePickupBlasts(); }
    /** Drain hits from the last projectile/attack phase before a level transition freezes
     * simulation. No AI, collision, or time advances, and each reward is consumed once. */
    void settleRewards(std::span<const PlayerRuntime> players, OpponentHost& host,
                       BossEncounterHost& encounter);
    /** Releases a chest's Death record; count distinguishes red from black. */
    bool releaseDeath(s32 record, const Vec3& position, s32 count);
    /** Stops the contact-drain loop while simulation is paused. */
    void stopDeathSound();
    /** Complete desired combat audio, once after the entire native tick. */
    void gatherLoops() const;
    static std::vector<EnemyView> enemyViews(std::span<const PlayerRuntime> players);
    /** enemyViews with the players the developer console hides marked invisible: not sought
     * or shot at, still struck by what they walk into. */
    std::vector<EnemyView> views(std::span<const PlayerRuntime> players) const;
    void setUnseen(s32 player, bool unseen);
    bool unseen(s32 player) const;
    /** Lends the swarm another level's roster entries, for the voices of kinds the developer
     * console brings in from it; kinds the level has keep its own. */
    void borrowRoster(std::span<const LevelEnemy> entries);
    /** Feedback-only view of the roster; imported worlds have no GDL level record,
     * so their borrowed voices use an otherwise empty non-boss view. */
    const LevelInfo* levelNow() const;
    Vec3 resolveMovement(const PlayerActor& player, const Vec3& from, const Vec3& to) const;
    /** Routes a contact by player identity; breath uses a shared quarter-second gate. */
    static bool applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                 OpponentHost& host, const CombatCover& cover);
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
    using GeneratorPublisher = std::function<std::optional<u32>(const GeneratorFeedbackEmission&)>;
    /** Shared native damage/reward path with a prepared-art publisher. False
     * requires the caller's complete combat rewind: the generator was already
     * struck, but rewards and brood detachment have not yet been committed. */
    bool strikeGenerator(s32 id, f32 power, s32 byPlayer, std::span<const PlayerRuntime> players,
                         const GeneratorPublisher& publish);
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
    const LegendsActors& importedActors() const { return m_importedActors; }
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
    const CombatantProjectiles& combatantProjectiles() const { return m_combatantProjectiles; }
    /** All retained coordinator values, not a projection of visible actors.
     * Child populations have their own native visitors. */
    bool appendNativeFollowupDigest(NativeStateDigest& digest) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    BossMeters& meter() { return m_bossMeter; }
    const BossMeters& meter() const { return m_bossMeter; }
    /** The lights the swarm's lobs and their bursts give off. */
    void lights(std::vector<PointLight>& out) const;
    /** How loud a sound made at `at` is heard: full within twenty of the nearest standing
     * player, fading to nothing at seventy (sndFxPlay3DAtten). */
    static f32 attenuation(const Vec3& at, std::span<const Vec3> hearers);

private:
    friend class SimulationObserver;
    friend class OpponentCombatCheckpoint;
    // Identifies the lifetime of the borrowed level services, not gameplay state.
    u64 m_bindingGeneration = 1;
    /** Plays `name` at `level` of 255 as heard from where the players stand. */
    SoundHandle playAt(std::string_view name, f32 level, const Vec3& at);
    void landEnemyMissiles(std::span<PlayerRuntime> players, OpponentHost& host);
    /** One native landing in world/target/effect/sound/hurt/burst order. */
    bool dispatchEnemyMissileHit(const EnemyMissileHit& /*hit*/,
                                 std::span<PlayerRuntime> /*players*/, OpponentHost& /*host*/);
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
        bool operator==(const Cloud&) const = default;
    };
    /** The garm brood's death shot: its burst, then the shot that flies on from it. */
    struct DeathShot {
        u32 burst = 0; ///< the effect held where the corpse lay; nought once it is over
        s32 kind = kGarmBroodKind;
        Vec3 position{0.0f};
        Vec3 velocity{0.0f};
        bool flying = false; ///< the shot's own effect has been sent on its way
        bool operator==(const DeathShot&) const = default;
    };
    void fireDeathShot(const EnemyDeathShot& shot);
    void advanceDeathShots();
    void hearFrom(std::span<const PlayerRuntime> players);
    void beginOrdinaryFrame(f32 seconds, std::span<PlayerRuntime> players);
    void countOrdinaryPlayers(std::span<const PlayerRuntime> players);
    void deliverLessons(std::span<const PlayerRuntime> players, OpponentHost& host);
    void applyEnemyBlow(const EnemyBlow& blow, std::span<PlayerRuntime> players,
                        OpponentHost& host);
    void awardEnemyLosses(OpponentHost& host);
    void awardBossLosses(std::span<const PlayerRuntime> players, OpponentHost& host,
                         BossEncounterHost& encounter);
    void awardCritterLosses(std::span<const PlayerRuntime> players, OpponentHost& host);
    void showCritterCue(const CombatCue& cue, ItemArchive* archive, bool ofBoss);
    void advanceCritters(s32 ticks, f32 seconds, std::span<const EnemyView> views, bool timeStopped,
                         std::span<const CombatantObstacle> walkedInto,
                         std::span<PlayerRuntime> players);
    void followCritterEffects(std::span<const PlayerRuntime> players);
    void updateDeaths(std::span<PlayerRuntime> players, OpponentHost& host);
    void clearDeaths();
    void finishSummons(std::span<const EnemyView> players);
    using Placement = PlacedEnemies::Placement;
    void standPlacements(const std::optional<ViewSet>& views);
    void stand(const Placement& placement);
    /** Whether the camera sees a sphere from close enough for a placement to stand. */
    bool inView(const Vec3& at, f32 radius) const;
    /** Stands the statues woken and seen whose ACTIVE has played out as the great ones they
     * are, and wakes what the triggers ask. */
    void updateStatues(s32 ticks, f32 seconds, std::span<PlayerRuntime> players);
    void playBirths();
    PlacedEnemies m_placements;
    CritterStatues m_statues;
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
        bool operator==(const Bag&) const = default;
    };
    void updateBags(f32 seconds);
    void releaseBag(const Bag& bag);
    /** Sends a bag up where it lies, to open where it lands. */
    void throwBag(Bag bag);
    /** Leaves what the mechanics drop for a fallen body the way a great one's carried
     * pickup is thrown. */
    class BagThrow final : public BagDrops {
    public:
        explicit BagThrow(LevelOpponents& owner) : m_owner(owner) {}
        void leave(std::string_view item, const Vec3& at) override;

    private:
        LevelOpponents& m_owner;
    };
    /** Sounds the opponents' cues from the level's own banks, the way the original does. */
    class NativeCues final : public OpponentCues {
    public:
        explicit NativeCues(LevelOpponents& owner) : m_owner(owner) {}
        void enemyCue(const EnemyCue& cue) override;
        void missileLanded(std::string_view sound, f32 level, const Vec3& at) override;
        void suicideBang(std::string_view sound, const Vec3& at) override;
        void shotSound(const Shot& shot) override;
        void enemyFeedback(const EnemyFeedback& feedback, std::string_view sound,
                           const Vec3& at) override;
        CueOwner enemyMelee(s32 kind, s32 tier, const std::optional<std::string>& sound) override;
        void generatorFeedback(const GeneratorEvent& event, std::string_view sound) override;
        void combatantSound(std::string_view sound, const Vec3& at, bool attenuated,
                            CombatantPool pool) override;
        void deathCue(const DeathEvent& cue) override;
        CueOwner deathContact(bool contact) override;
        /** Nothing to do: the drain's own sound is stopped by its caller. */
        void deathContactReleased() override {}

    private:
        LevelOpponents& m_owner;
    };
    NativeCues m_nativeCues{*this};
    /** Where the cues go on the level as it stands: an imported level's first, the rest to
     * the native cues. Callers have checked the borrowed services. */
    ImportedOpponentCues cues();
    /** A great one slain lets what it carried go, a gargoyle its piece of the wings if it
     * carried nothing (CritterDropItem), with the lesson its kind teaches. */
    void dropCarried(const CombatLoss& loss, std::span<const PlayerRuntime> players,
                     OpponentHost& host);
    std::optional<Resources> m_resources;
    void refreshBorrowedRoster();
    void bindImportedSounds();
    void openImportedBoss(const EnemyScales& scales);
    void openImportedActors();
    void updateImportedActors(f32 seconds, std::span<const EnemyView> views, bool timeStopped);
    std::optional<LevelInfo> m_levelNow; ///< the level's record with the borrowed roster
    Enemies m_enemies;
    MechanicsFacade* m_mechanics = nullptr;  ///< borrowed until close; never simulation state
    std::optional<s32> m_partyCountOverride; ///< the scene's, applied each count
    Generators m_generators;
    std::vector<std::pair<u32, s32>> m_importedGenerators;
    struct ImportedGeneratorSound {
        s32 kind = -1;
        s32 realm = -1;
        std::string level;
    };
    std::vector<ImportedGeneratorSound> m_importedGeneratorSounds;
    std::unique_ptr<TextureSet> m_importedBossTextures;
    LegendsActors m_importedActors;
    Critters m_critters;
    static constexpr usize kPlayerIds = 4;   ///< players are numbered 0 to 3
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
        bool operator==(const CritterEffect&) const = default;
    };
    struct MoveEffect {
        u32 effect = 0;
        s32 critter = -1;
        bool ofBoss = false;
        bool operator==(const MoveEffect&) const = default;
    };
    SoundHandle m_deathSound = kNoSound;
    bool m_resumeDeathSound = false;  ///< offline output was stopped for pause
    std::vector<SoundHandle> m_yells; ///< suicides' cries, cut short when one is struck down
    struct Yell {
        FrameEventEmitter emitter;
        u32 instance = 0; ///< simultaneous cries from this same enemy remain distinct
        SoundCueToken start;
        SpeechTiming timing;
        f32 volume = 1;
        Vec3 position{0.0f}; ///< where it began, panned from each machine's own listener
        bool operator==(const Yell&) const = default;
    };
    struct YellClock {
        u64 source = 0;
        u32 next = 1;
        bool operator==(const YellClock&) const = default;
    };
    /** Holds the swarm, the great ones and their missiles at the shrinkers' scale. */
    void shrinkOpponents(std::span<const PlayerRuntime> players);

    /** What players' blows on generators earned: who, how much, whether destroyed; drained
     * on the next settling. */
    struct GeneratorReward {
        s32 player = -1;
        s32 experience = 0;
        bool destroyed = false;
        bool operator==(const GeneratorReward&) const = default;
    };
    /** Mutable coordinator state; archives, services and playback handles stay outside. */
    struct Simulation {
        std::vector<Cloud> clouds;
        std::vector<DeathShot> deathShots;
        /** Carried pickup indices, by critter pool slot. */
        std::array<std::optional<usize>, Critters::kMost> carried{};
        std::vector<Bag> bags;
        u32 bagSeed = 1;
        std::vector<CombatantRam> barrelRams;
        std::vector<RockHit> rockHits;
        /** Native hit_streak: hits on bred enemies since destroying a generator. */
        std::array<s32, kPlayerIds> hitStreak{};
        /** Pending lessons in dispatch order, by player identity. */
        std::vector<std::pair<s32, s32>> lessons;
        std::vector<CritterEffect> critterEffects;
        std::vector<MoveEffect> moveEffects;
        std::vector<u32>
            cueEffects; ///< all emitted cues, including detached effects borrowing artwork
        std::array<u32, Enemies::kMost> deathEffects{};
        bool deathContact = false;
        SoundCueToken deathSound;
        std::vector<Yell> yells;
        std::array<YellClock, Enemies::kMost> yellClocks;
        std::vector<Vec3> hearers; ///< where the standing players are, for how loud
        /** Last applied shrink scale, including the logical unshrink sound edge. */
        f32 shrink = 1.0f;
        std::array<f32, 4> critterExperienceOwed{};
        std::vector<GeneratorReward> generatorRewards;
        std::array<bool, 4> unseen{};
        std::vector<LevelEnemy> borrowedRoster;
        bool operator==(const Simulation&) const = default;
    };

public:
    class LocalSnapshot {
        friend class LevelOpponents;
        Simulation m_values;
        PlacedEnemies::LocalSnapshot m_placements;
        CritterStatues::LocalSnapshot m_statues;
        Enemies::LocalSnapshot m_enemies;
        Generators::LocalSnapshot m_generators;
        Critters::LocalSnapshot m_critters;
        Bosses::LocalSnapshot m_bosses;
        BossMeters::LocalSnapshot m_meter;
        EnemyMissiles::LocalSnapshot m_missiles;
        CombatantProjectiles::LocalSnapshot m_projectiles;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    /** Capture/restore the same loaded population. The caller rewinds world, players
     * and effect trees together; no resource rebinding or sound commands occur here. */
    bool saveLocal(LocalSnapshot& saved) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    /** Exact native values from this same loaded population, including queued
     * outputs and inactive slots; mixer handles and loaded artwork are external. */
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    bool appendLocalFollowupDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    static void appendFollowupValues(NativeStateDigest& digest, const Simulation& values);
    Simulation m_sim;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
};
} // namespace gdl::game
