#pragma once
#include <array>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/CombatEvents.h"
#include "game/enemies/CombatantAssets.h"
#include "game/enemies/CombatantGaze.h"
#include "game/enemies/CombatantProjectile.h"
#include "game/enemies/CritterArea.h"
#include "game/enemies/CritterPatrol.h"
#include "game/enemies/Enemies.h"
#include "game/world/HazardSurfaces.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {
/** Something of the level's where a great one walks (CritterCollideItems, fn_8005D5C8): it
 * stops the great one, but a golem or gargoyle walks through a chest and breaks what is
 * breakable, stopping only while it stands or as it blows up. */
struct CombatantObstacle {
    enum class Kind : u8 { Blocks, Chest, Breakable };
    Obstacle box;
    Kind kind = Kind::Blocks;
    s32 id = -1; ///< a breakable's: a barrel's index, or a generator's from 2000
    s32 health = 0;
    s32 armor = 0;
    bool explodes = false;
};

/** A great one's blow on something it walked into, for the level to deal. */
struct CombatantRam {
    s32 id = -1;
    f32 damage = 0.0f;
};

/** One fighter. Executes authored moves with family policy, without owning a population.
 * Assets are borrowed and must outlive this actor and any emitted projectiles/effects. */
class Combatant {
public:
    Combatant() = default;
    Combatant(const Combatant&) = delete;
    Combatant& operator=(const Combatant&) = delete;
    Combatant(Combatant&&) = delete;
    Combatant& operator=(Combatant&&) = delete;
    ~Combatant() = default;
    static constexpr f32 kBlockShare = 0.25f;
    static constexpr f32 kKillShare = 0.2f;
    static constexpr f32 kRoarAfter = 50.0f;
    static constexpr std::array<f32, 5> kRoarShares{1.0f, 1.0f, 1.5f, 2.0f, 2.0f};
    static constexpr f32 kRoarMemory = 3.0f;
    static constexpr s32 kFlashTicks = 4; ///< two 30 Hz frames
    static constexpr u32 kFlashesWhole = 0x100000 | 0x200 | 0x100 | 0x20;
    static constexpr f32 kUnderLevelLoss = 0.02f;
    /** A boss's share of harm by how many are in the game (lbl_8011AEC0). */
    static constexpr std::array<f32, 5> kBossShares{1.0f, 1.0f, 0.5f, 0.3f, 0.2f};
    static constexpr s32 kTicksPerSecond = 60;
    /** Replaces the actor state; pending events survive until taken or explicitly cleared. */
    bool spawn(CombatantAssets& stock, s32 id, const Vec3& position, f32 yaw,
               const WorldCollision* collision, const EnemyScales& scales, char realm);
    /** Independently controlled branches share assets and the body's placement. */
    const Combatant* child(s32 id) const;
    usize childCount() const { return m_children.size(); }
    void clear();
    /** Static stage attachment points supplied by the encounter, not player targets. */
    void setArenaAnchors(std::span<const Mat4> anchors);
    void setArenaTargets(std::span<const CombatArenaTarget> targets);
    bool raisesArenaRocks() const;
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                std::span<const Combatant> peers = {}, bool timeStopped = false);
    /** Follows the supporting moving floor without advancing combat or animation. */
    void syncFloor();
    /** Healing credit after armor/party scaling, before level and hit-node adjustments. */
    f32 hurt(const EnemyHit& hit, s32 partId = -1);
    /** Records a routed hit's nominal damage to a player, after contact/cooldown gates. */
    void damagedPlayer(s32 player, f32 amount, s32 partId = -1);
    /** The level's harmful surfaces, which hurt it where it walks against or onto them
     * (CritterWorldDamage); borrowed, none for nothing. */
    void setHazards(const HazardSurfaces* hazards) { m_hazards = hazards; }
    /** What of the level's stands where it walks this update. */
    void setObstacles(std::span<const CombatantObstacle> items) { m_obstacles = items; }
    void setSwarm(std::span<const EnemyBody> bodies) { m_swarm = bodies; }
    std::vector<CombatTrample> takeTramples() { return std::exchange(m_tramples, {}); }
    std::vector<CombatantRam> takeRams() { return std::exchange(m_rams, {}); }
    std::vector<CombatPush> takePushes() { return std::exchange(m_pushes, {}); }
    /** Sets it walking the level's lookouts from the chained one nearest it, within ten
     * (CritterNewInst's round for a general); with `sight` over nought it takes only a player
     * whose target score is within it meanwhile (the placement's radius at the level's sight
     * scale, CritterGetSingleTargetPlayer). `route` is borrowed for the round. */
    void startPatrol(const LookoutRoute* route, f32 sight = 0.0f);
    bool patrolling() const { return m_actor.patrol.active(); }
    /** The lookout it makes for, by its place in the route; -1 for none. */
    s32 lookout() const { return m_actor.patrol.lookout(); }
    void freeze(s32 ticks);
    void blind(s32 ticks);
    void curb(f32 seconds);
    void resize(f32 scale);
    /** The scale the party's enemy shrinkers hold it at (`EnemyShrink`): drawn and shadowed
     * at it over its own size; a great one, never a boss, then takes double and deals half. */
    void setShrink(f32 scale);
    f32 shrink() const { return m_actor.shrink; }
    void tint(Color color) { m_actor.tint = color; }
    void hold(bool held);
    /** While a legend item's rite runs, a boss takes its harm whole, however many play. */
    void takeFullHarm(bool full) { m_fullHarm = full; }
    void roar();
    bool present() const { return m_actor.state != State::Inactive; }
    bool alive() const { return m_actor.state == State::Active; }
    bool dying() const { return m_actor.state == State::Dying; }
    s32 id() const { return m_id; }
    CombatantKind kind() const {
        return data() != nullptr ? data()->kind() : CombatantKind::Unknown;
    }
    f32 health() const { return m_actor.health; }
    bool flashing() const { return m_actor.flashTicks > 0; }
    static f32 roarThreshold(s32 players);
    f32 maxHealth() const { return m_actor.maxHealth; }
    const Vec3& position() const { return m_actor.position; }
    f32 yaw() const { return m_actor.yaw; }
    f32 radius() const { return data() != nullptr ? data()->radius() : 0; }
    s32 target() const { return m_actor.target; }
    bool moveDone() const { return m_actor.moveDone; }
    bool frozen() const { return m_actor.frozenTicks > 0; }
    bool blinded() const { return m_actor.blindTicks > 0; }
    bool curbed() const { return m_actor.curbSeconds > 0; }
    f32 scale() const { return m_actor.scale; }
    s32 moveType() const;
    std::string_view moveName() const;
    std::string form() const;
    const CritterData* data() const;
    ItemArchive* archive();
    std::optional<Mat4> nodeTransform(std::string_view node) const;
    std::optional<Mat4> rootTransform() const;
    /** Animated hit volumes; movement uses only solid nodes plus the root fallback. */
    std::vector<MissileTarget> bodyTargets(bool solidOnly = false) const;
    std::optional<f32> contactDistance(const Vec3& from, const Vec3& to, f32 radius) const;
    bool within(const Vec3& centre, f32 radius) const;
    bool reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;
    /** Its shadow on the floor under it, when its type lies one. */
    void drawShadow(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                    const WorldLighting& lighting, f32 presentationAlpha = -1) const;
    /** Its body, and the bar over it when its type hangs one, turned to `camera`. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr, const CameraFrame* camera = nullptr,
              const Texture* hitFlash = nullptr, f32 presentationAlpha = -1) const;
    /** The GMETER bar's placement and its nodes' matrices, while it hangs over the body. */
    std::optional<std::pair<Mat4, std::vector<Mat4>>> meterPose(const CameraFrame* camera,
                                                                f32 presentationAlpha = -1) const;
    std::vector<CombatBlow> takeBlows();
    std::vector<CombatGrab> takeGrabs();
    std::vector<CombatLoss> takeLosses();
    std::vector<CombatCue> takeCues();
    std::vector<CombatSpew> takeSpews();
    std::vector<CombatShot> takeShots();
    std::vector<CombatArenaActivation> takeArenaActivations();

private:
    static constexpr usize kPlayerSlots = 4;
    static constexpr f32 kAngerMemory = 15.0f;
    struct PlayerDamage {
        f32 dealt = 0;
        f32 received = 0;
        f32 dealtTime = 0;
        f32 receivedTime = 0;
    };
    struct Target {
        s32 player = -1;
        f32 distance = 0;
        f32 score = 0;
        f32 inverseAnger = 1;
    };
    enum class State : u8 { Inactive, Active, Dying };
    struct HitNode {
        f32 health = 0;
        s32 flashTicks = 0;
        bool broken = false;
        std::optional<NodePose> heldPose;
    };
    struct Actor {
        State state = State::Inactive;
        bool hidden = false;
        bool forcedPattern = false;
        bool childrenIntact = true;
        bool smoothValid = false;
        CombatantAssets* stock = nullptr;
        const CritterData* definition = nullptr;
        const Combatant* parent = nullptr;
        std::optional<usize> branch;
        f32 health = 0.0f;
        f32 burnGap = 0;
        f32 maxHealth = 1.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        struct Floor {
            s32 object = -1;
            Vec3 local{0};
        };
        std::optional<Floor> floor;
        f32 yaw = 0.0f;
        f32 initialYaw = 0.0f;
        Vec3 homePosition{0.0f}; ///< floor-space home anchor, independent of current position
        Vec3 initialRoot{0.0f};  ///< geometry's saved base, distinct from an explicit roaming home
        Vec3 push{0.0f, 0.0f, 0.0f};
        s32 target = -1;
        f32 targetDistance = 100000.0f;
        std::vector<Target> targets;
        std::array<PlayerDamage, kPlayerSlots> playerDamage{};
        s32 move = -1;       ///< the move playing
        s32 moveTarget = -1; ///< player locked by MoveSetup, independent of the fresh roster
        s32 pattern = -1;
        f32 finishedSeconds = 0.0f;
        f32 age = 0.0f;
        std::vector<f32> moveTimes;
        std::vector<f32> patternTimes;
        usize patternStep = 0;
        std::vector<s32> struckThisMove; ///< players already hurt by the move playing
        std::vector<CritterArea> areas;
        std::vector<Mat4> arenaAnchors;
        std::vector<CombatArenaTarget> arenaTargets;
        CritterPatrol patrol;     ///< its round of the lookouts, while it has one
        s32 lastArenaTarget = -1; ///< position in the collected stage roster, not the item id
        f32 hurtPending = 0.0f;
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        f32 roarOwed = 0.0f; ///< damage taken toward the next roar
        f32 sinceHurt = 0.0f;
        s32 flashTicks = 0;
        const CombatEffectDefinition* skin = nullptr;
        f32 skinAge = 0;
        std::vector<HitNode> hitNodes;
        f32 alpha = 1.0f;
        Color tint = Color::white();
        f32 scale = 1.0f;
        f32 shrink = 1.0f;       ///< the enemy shrinkers' scale on top of its own
        s32 frozenTicks = 0;     ///< a legend item's: it stands still this long
        s32 blindTicks = 0;      ///< and finds no one this long
        f32 curbSeconds = 0.0f;  ///< over nought, its curbed attacks are refused
        bool held = false;       ///< keeps to its stance between moves
        bool roarWanted = false; ///< roars as soon as it may
        bool moveEffect = false; ///< an emitted effect needs cancellation on the next move
        bool moveDone = false;
        u32 soundsGiven = 0; ///< bits: the move's sound, its second, each strike's
        bool arenaCollected = false;
        s32 shotFrame = -1;
        std::optional<Vec3> attackTarget; ///< captured by a targeted-area move, not a homing point
        std::optional<Vec3> stepTarget;   ///< latest ready-step target, retained if sight is lost
        std::optional<Vec3> patrolAim;    ///< the lookout it makes for this update
        s32 grabbed = -1;
        s32 grabMove = -1;
        std::string grabNode;
        Vec3 grabOffset{0};
        AnimationPlayer player;
        TreePose pose;
        TreePose smooth;
        struct Presentation {
            bool valid = false;
            State state = State::Inactive;
            u64 generation = 0;
            u32 sequence = 0;
            Vec3 position{0};
            f32 yaw = 0;
            f32 frame = 0;
            TreePose pose;
        } presentation;
        struct Attachment {
            AnimationPlayer player;
            TreePose pose;
            Mat4 world{1}; ///< fixed world parent when ADDA does not follow a node
            f32 previousFrame = 0;
            u64 previousGeneration = 0;
        };
        std::vector<Attachment> attachments;
        CombatantGaze gaze; ///< the turn of its head and eyes to its target
    };

    bool startMove(Actor& critter, usize index, bool recordUse = true);
    void updateActor(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                     std::span<const Combatant> peers, bool timeStopped);
    void rememberFloor();
    /** What it deals of `amount`: half while shrunk, unless it is a boss. */
    f32 dealt(f32 amount) const;
    bool spawnActor(CombatantAssets& stock, const CritterData& definition, s32 id,
                    const Vec3& position, f32 yaw, const WorldCollision* collision,
                    const EnemyScales& scales, char realm);
    void updateChildren(s32 ticks, f32 seconds, std::span<const EnemyView> players);
    static void updateHitFlashes(Actor& actor, s32 ticks);
    /** Turns its head and eyes to its target on top of the pose (CritterLookAtPlayer): not
     * at all through its entrance, and back to the animation while it dies, holds its head
     * to a move, is frozen or blinded. A head with no target of its own takes the body's. */
    static void aimGaze(Actor& critter, f32 seconds, std::span<const EnemyView> players);
    void inheritBodyPose();
    void synchronizeChild();
    void collectChildEvents(Combatant& part);
    f32 hurtActor(const EnemyHit& hit);
    f32 damageNode(s32 index, f32 amount, u32 flags);
    static bool nodeAvailable(const Actor& actor, std::string_view name);
    static bool nodeRemoved(const Actor& actor, usize node);
    static void holdBrokenPoses(Actor& actor);
    static void drawNodeState(const Actor& actor, const Texture* flash);
    static void drawBrokenModels(const Actor& actor, RenderDevice& device, const Mat4& clip,
                                 const WorldLighting& lighting, const Texture* frozen,
                                 const Texture* flash, const Mat4& placement, const TreePose& pose);
    void capturePresentation();
    static TreePose smoothPose(const Actor& actor);
    static f32 presentationBlend(const Actor& actor, f32 alpha);
    static TreePose presentationPose(const Actor& actor, f32 alpha);
    static f32 presentationFrame(const Actor& actor, f32 alpha);
    static Mat4 presentationModel(const Actor& actor, f32 alpha);
    std::vector<MissileTarget> ownTargets(bool solidOnly) const;
    void loseHealth(f32 amount);
    void chooseMove(Actor& critter, std::span<const EnemyView> players);
    static std::optional<usize> bestMove(const Actor& critter, std::span<const EnemyView> players);
    /** The first step move that goes anywhere, for a round of the lookouts with no player in
     * sight (CritterLookForReady's waypoint pick). */
    static std::optional<usize> patrolStep(const Actor& critter);
    /** How a player scores as a target: its distance, doubled unless it lies squarely ahead
     * (CritterCalcTargetScore). */
    static f32 targetScore(const Actor& critter, const Vec3& position);
    bool choosePatternAttack(Actor& critter, std::span<const EnemyView> players);
    static s32 attackTarget(const Actor& critter, const TargetCriteria& criteria,
                            std::span<const EnemyView> players, bool fallback = false);
    static f32 attackRate(const Actor& critter);
    static bool supportsArea(const AttackDefinition& damage, const CombatEffectDefinition* sound);
    std::optional<f32> startArea(Actor& critter, s32 id, const AttackDefinition& damage,
                                 std::string_view node,
                                 std::optional<Mat4> worldParent = std::nullopt);
    void eruptArena(Actor& critter, s32 id, const AttackDefinition& damage,
                    std::span<const EnemyView> players);
    void updateAreas(Actor& critter, s32 id, std::span<const EnemyView> players);
    /** Whether a legend item's curb keeps the move from it. */
    static bool curbedMove(const Actor& critter, const MoveDefinition& move);
    /** Sets off sound record `index` (and what it links to) at `position`. */
    void cue(Actor& critter, s32 id, s32 index, const Vec3& position,
             std::optional<std::string_view> node = std::nullopt,
             const AttackDefinition* damage = nullptr, std::optional<s32> player = std::nullopt);

    std::vector<CombatCue> m_cues;
    std::vector<CombatGrab> m_grabs;
    void grab(Actor& actor, const MoveDefinition& move, const AttackDefinition& damage,
              bool release, std::span<const EnemyView> players);
    void carryGrab(Actor& actor, std::span<const EnemyView> players);
    std::vector<CombatSpew> m_spews;
    std::vector<CombatShot> m_shots;
    std::vector<CombatArenaActivation> m_arenaActivations;
    void shoot(const Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
               std::span<const EnemyView> players);
    void plant(const Actor& critter, s32 id, s32 damageIndex);
    void strikeWith(Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
                    std::span<const EnemyView> players);
    static Vec3 partPosition(const Actor& critter, std::string_view node);
    static Mat4 partTransform(const Actor& critter, std::string_view node);
    static Mat4 attachmentTransform(const Actor& critter, std::string_view node);
    static Mat4 modelTransform(const Actor& critter);
    void carry(Actor& critter, f32 seconds, const MoveDefinition* move,
               std::span<const EnemyView> players, std::span<const Combatant> peers);
    static void chooseTarget(Actor& critter, std::span<const EnemyView> players);
    void chooseFamilyTargets(std::span<const EnemyView> players);
    static void selectFirstTarget(Actor& critter);
    static f32 inverseAnger(const Actor& critter, usize player);
    static f32 targetClock(const Actor& critter);
    static void rememberDamage(f32& total, f32& lastTime, f32 now, f32 amount);
    bool blockedByItems(Actor& critter, const Vec3& to);
    bool blockedBySwarm(Actor& critter, const Vec3& to);
    static const EnemyView* viewOf(std::span<const EnemyView> players, s32 player);

    Actor m_actor;
    std::vector<std::unique_ptr<Combatant>> m_children;
    s32 m_id = -1;
    const WorldCollision* m_collision = nullptr;
    const HazardSurfaces* m_hazards = nullptr; ///< borrowed from the level
    EnemyScales m_scales;
    std::span<const CombatantObstacle> m_obstacles;
    std::span<const EnemyBody> m_swarm;
    std::vector<CombatTrample> m_tramples;
    std::vector<CombatantRam> m_rams;
    std::vector<CombatPush> m_pushes;
    char m_realm = 'G';
    std::vector<CombatBlow> m_blows;
    bool m_fullHarm = false;
    std::vector<CombatLoss> m_losses;
    std::minstd_rand m_arenaRandom;
};
} // namespace gdl::game
