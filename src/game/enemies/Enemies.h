#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"
#include "game/world/BlobShadow.h"
#include "game/world/HazardSurfaces.h"
#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {

/** The level's scales on its enemies. */
struct EnemyScales {
    f32 health = 1.0f;
    f32 speed = 1.0f;
    f32 sight = 1.0f;
    f32 damage = 1.0f;
    f32 missileRate = 1.0f;     ///< how long casters wait between casts
    f32 missileAim = 0.0f;      ///< how far up or down a thrower's aim strays
    f32 playerLevel = 0.0f;     ///< the level the place is meant for; none when nought
    bool bossEncounter = false; ///< applies to every opponent in the arena, not just the boss
    s32 players = 1;            ///< how many are in the game: a boss's share of harm and worth
};

/** Protection that returns a swarm enemy's melee blow to its attacker. */
enum class EnemyMeleeWard : u8 { None, HandOfDeath, HealthVamp };

/** A player as the enemies see one. */
struct EnemyView {
    s32 player = -1;
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< its feet
    f32 radius = 1.0f;
    f32 height = 6.0f;
    s32 level = 1;
    bool hidden = false;    ///< not to be seen or sought
    bool captured = false;  ///< already parented to another combatant, not a new grab candidate
    bool invisible = false; ///< not a sight target, but still vulnerable to contact and hazards
    EnemyMeleeWard meleeWard = EnemyMeleeWard::None;
    bool antiDeath = false;
    bool reflects = false;    ///< its armour turns the swarm's missiles back
    bool it = false;          ///< tagged by IT: every enemy that can see it goes for it
    bool damageable = true;   ///< action/partner immunity; still visible to enemy targeting
    bool recentlyHit = false; ///< shared effect-hit grace makes ordinary critters prefer others
    std::optional<f32> collisionHeight = std::nullopt; ///< native centre, else half the height
    bool blockableAttack = false; ///< slow/power attack groups that provoke a critter's BLOCK
    std::optional<Vec3> decoy = std::nullopt; ///< swarm range/bearing, never a collision body
};

/** A blow an enemy has landed on a player. */
struct EnemyBlow {
    s32 player = -1;
    s32 kind = 0;
    s32 tier = 1;
    f32 damage = 0.0f;
    bool power = false;               ///< the stronger every-eighth blow
    u32 flags = 0;                    ///< damage modifiers, including low attacks and knockback
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< from the enemy to the player
    EnemyMeleeWard ward = EnemyMeleeWard::None; ///< returned damage; only Health Vamp also heals
};

/** An enemy hurt or killed by a player, and what that is worth. */
struct EnemyLoss {
    s32 enemy = -1;
    s32 kind = 0;
    s32 tier = 1;
    s32 player = -1;
    s32 experience = 0;
    bool killed = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** A living swarm member's collision body, independent of its render/aiming bounds. */
struct EnemyBody {
    s32 id = -1;
    Vec3 centre{0};
    f32 radius = 0;
    f32 halfHeight = 0;
};

/** Ordered generator bookkeeping, including patrol offspring's change to future births. */
struct EnemyGeneratorEvent {
    enum class Kind : u8 { Born, Detached, PatrolHit, PatrolDetached };
    s32 generator = -1;
    Kind kind = Kind::Born;
};

/** What a player's hit carries besides damage: the original's damage-type bits that matter. */
struct EnemyHit {
    static constexpr u32 kKnockBack = 0x10;
    static constexpr u32 kKnockDown = 0x20;
    static constexpr u32 kFloors = 0x10160; ///< any of these throws the enemy down
    static constexpr u32 kMagic = 0x200;    ///< a magic hit over ten also throws it down

    f32 damage = 0.0f;
    u32 flags = 0;
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< the way the hit travels
    s32 player = -1;                  ///< who dealt it; -1 for the world
    s32 level = 1;                    ///< the character's level, against the place's
    std::optional<Vec3> where;        ///< where it landed, when that is known
    bool close = false;               ///< a blow struck in the hand, not a missile
    bool antiDeath = false;
    bool selfInflicted = false; ///< a suicide's own fuse: no cry, burst or death skin
    s32 node = -1;              ///< authored collision-part index within the struck combatant
};

/** Where an enemy is asked to appear: about `position`, facing `direction`. */
struct EnemySpawn {
    /** do_items alternates the handedness of algorithm-14 births; zero for other births. */
    s32 zigZagSide = 0;
    enum class Priority : s8 { FreeSlotOnly = -1, Offscreen = 0, Visible = 1 };
    s32 kind = kGruntKind;
    s32 tier = 1;
    s32 algorithm = -1; ///< the kind's own when negative
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f};
    f32 clearance = 0.0f; ///< how far out from `position` it is set (a generator's height)
    s32 generator = -1;
    bool placed = false;                     ///< set exactly where asked, as a level's placement is
    bool asleep = false;                     ///< a placement of no strength waits to be woken
    f32 throwInterval = 1.0f;                ///< seconds, before the level's missile-rate scale
    Priority priority = Priority::Offscreen; ///< replacement permission, independent of strength
    /** A placement's own sight radius, before the level's scale (the float after its strength
     * and way, SetItem items.c 5569); nought for the kind's thirty. */
    f32 sight = 0.0f;
};

/** The garm brood's death shot (kill_enemy, fn_8004F1DC): as the corpse goes, its burst is
 * sent from where it lay toward the player it was after, else the first standing. */
struct EnemyDeathShot {
    s32 enemy = -1;
    s32 kind = kGarmBroodKind;
    Vec3 position{0.0f, 0.0f, 0.0f};  ///< where the body lay
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< toward the player, unit length
};

/** A blast one of the swarm goes up in. */
struct EnemyBurst {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    s32 enemy = -1;
    s32 kind = -1;             ///< whose fragments scatter when it explodes
    bool silencesYell = false; ///< set off by a hit rather than its own fuse
};

/** A sound one of the swarm makes other than on being hit: a throw going off (the arrow's
 * or a bolt's), or a suicide's yell as it starts its run. */
struct EnemyCue {
    enum class Kind : u8 { Arrow, Bolt, Yell };
    Kind kind = Kind::Arrow;
    s32 enemyKind = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** The strengths a placement gives past the tiers: the variants. */
inline constexpr s32 kArcherStrength = 4;
inline constexpr s32 kBomberStrength = 5;
inline constexpr s32 kSuicideStrength = 6;

/** The way an enemy of `kind` bred or placed at `strength` goes about, given the way its
 * generator or placement names (fn_8004F87C, then init_enemy_vars): a way out of range is
 * the kind's own; a small kind prowls, mirrored when `mirrored`, unless told which; a way of
 * nought is filled in by strength (a strength-three caster casts, the variants throw, lob
 * and run at the party, the rest chase); ways one and ten are nought and seven. */
s32 resolvedWayOf(s32 kind, s32 strength, s32 way, bool mirrored);

/**
 * The swarm: the level's ordinary enemies, up to twenty-five of them, each with the mind the
 * original gives its kind. They see the nearest player within their sight, chase it hugging
 * the corners they bump into, and strike on touch, the blow landing as the swing ends whether
 * or not the player is still there. Struck, they flinch or are thrown down, and, killed, are
 * worth experience to the one who did it.
 */
class Enemies {
public:
    static constexpr s32 kMost = 25;
    static constexpr f32 kBaseSight = 30.0f;
    static constexpr f32 kWallRadiusScale = 1.5f; ///< the body keeps this much off walls
    static constexpr f32 kMostPush = 40.0f;
    static constexpr f32 kPushDecay = 0.8f;
    static constexpr f32 kBlowGrowth = 1.5f;      ///< a power blow's share over an ordinary one
    static constexpr f32 kKnockBackHeight = 2.0f; ///< a body reaching above this can knock back
    static constexpr f32 kSuicideDamage = 50.0f;  ///< at the level's enemy damage
    static constexpr s32 kTicksPerSecond = 60;
    static constexpr s32 kPlacedStun = 30; ///< ticks a placement of ordinary strength stands still

    Enemies() = default;
    Enemies(const Enemies&) = delete;
    Enemies& operator=(const Enemies&) = delete;
    Enemies(Enemies&&) = delete;
    Enemies& operator=(Enemies&&) = delete;
    ~Enemies();

    /** Prepares to hold the level's enemies: their kinds' archives are loaded from
     * `unpackedRoot` as they are first asked for. */
    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, s32 most, const EnemyScales& scales, u32 seed);
    void close();
    /** The level's harmful surfaces, borrowed until close. */
    void setHazards(const HazardSurfaces* hazards) { m_hazards = hazards; }
    /** What the camera takes in, for what is on screen; none takes everything in. */
    void setView(std::optional<ViewVolume> view) { m_view = view; }
    /** The scale the party's enemy shrinkers hold the swarm at (`EnemyShrink`): shown and
     * shadowed at it, taking double and dealing half while under one. */
    void setShrink(f32 scale) { m_shrink = scale; }
    f32 shrink() const { return m_shrink; }
    /** How many stand in view by twice their radius, as of the last update (do_enemies);
     * without a view, all of them. */
    s32 inView() const { return m_inView; }
    /** The lookouts its patrollers walk between. */
    void setLookouts(LookoutRoute lookouts) { m_lookouts = std::move(lookouts); }

    /** Loads a kind's archive ahead of need; false when it is not there. */
    bool loadKind(s32 kind);
    bool kindLoaded(s32 kind) const;
    const ItemArchive* archiveOf(s32 kind) const;
    ItemArchive* archive(s32 kind);
    /** The prefix a kind's bodies are named by ("GRU") and the tree of a tier ("GRU1"). */
    const TreeInfo* treeOf(s32 kind, s32 tier) const;

    /** Puts one where asked, the way the original does: for a generator's, in one of the
     * clear octants about it; a placement's exactly there. Nullopt when there is no room or
     * no slot worth taking. */
    std::optional<s32> spawn(const EnemySpawn& spawn, std::span<const EnemyView> players,
                             std::span<const Obstacle> obstacles = {});
    /** Wakes a sleeping placement. */
    void wake(s32 id);
    /** Forgets everything of a generator that is gone. */
    void generatorGone(s32 generator);
    std::vector<EnemyGeneratorEvent> takeGeneratorEvents();

    /** Steps every mind and body `ticks` (`seconds` long) with the players where they are;
     * what the throwers let go flies in `missiles`, when given. Accumulates native
     * two-tick steps so render cadence cannot change AI, collision or floor damage.
     * Moving-floor attachment is refreshed even when no full step is due. */
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {}, class EnemyMissiles* missiles = nullptr,
                f32 missileSpeedScale = 1.0f, bool timeStopped = false);
    std::vector<EnemyBlow> takeBlows();
    std::vector<EnemyLoss> takeLosses();
    std::vector<EnemyBurst> takeBursts();
    std::vector<EnemyCue> takeCues();
    std::vector<EnemyFeedback> takeFeedback();
    std::vector<DeathEvent> takeDeathEvents();
    /** The garm brood's death shots since last asked, one as each corpse goes. */
    std::vector<EnemyDeathShot> takeDeathShots();
    /** The players IT has touched since last asked, in turn: each is now it. */
    std::vector<s32> takeTagged();
    bool draining(s32 id) const;
    /** Death departs after taking its victim's last health. */
    void finishDeath(s32 id, s32 player = -1);

    /** Deals a hit, queues its reward as a loss, and returns healing credit before level/armor
     * scaling; zero for rejected hits or Death, whose magic healing is a separate event. */
    f32 hurt(s32 id, const EnemyHit& hit);
    /** The enemies a missile can strike. */
    std::vector<MissileTarget> targets() const;
    /** The nearest live enemy whose body a blow sweeping from `from` to `to` with `radius`
     * touches. */
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    /** The live enemies within `radius` of `centre`. */
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    /** The live enemies a strike reaches. */
    std::vector<s32> reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;

    /** The shadows under the bodies (SHADOW1L1..3L1 by tier), for after the level's floors. */
    void drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                     const WorldLighting& lighting, f32 presentationAlpha = -1) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* hitFlash = nullptr, ItemArchive* weapons = nullptr,
              const CameraFrame* camera = nullptr, f32 presentationAlpha = -1);

    bool alive(s32 id) const;
    bool dying(s32 id) const;
    usize count() const; ///< live, dying included
    s32 kindOf(s32 id) const;
    s32 tierOf(s32 id) const;
    s32 generatorOf(s32 id) const;
    /** Whether a generator bred it, even one since gone (the original's birth_style 0). */
    bool bred(s32 id) const;
    f32 healthOf(s32 id) const;
    const Vec3& positionOf(s32 id) const;
    f32 yawOf(s32 id) const;
    f32 radiusOf(s32 id) const;
    f32 heightOf(s32 id) const;
    std::vector<EnemyBody> movementBodies() const;
    s32 targetOf(s32 id) const;
    /** Carry grounded bodies with moving scenery, without advancing AI or animation. */
    void syncFloors();
    s32 algorithmOf(s32 id) const;
    s32 pushCountOf(s32 id) const;
    s32 variantOf(s32 id) const;   ///< the strength it was placed at, four and over for a variant
    f32 sightOf(s32 id) const;     ///< how far it sees, at the level's scale
    s32 stunTicksOf(s32 id) const; ///< ticks it still stands still after appearing
    /** How solid it shows: one, or less while a veiling kind fades. */
    f32 opacityOf(s32 id) const { return 1.0f - m_enemies[static_cast<usize>(id)].veil / kVeiled; }
    static constexpr s32 kVeilingKind = 24; ///< the warlock (fn_8004D958's type 24)
    static constexpr f32 kVeiled = 255.0f;
    const EnemyAnimator* animatorOf(s32 id) const;
    const MindMemory& memoryOf(s32 id) const { return m_enemies[static_cast<usize>(id)].mind; }
    bool bumpedWallOf(s32 id) const { return m_enemies[static_cast<usize>(id)].bumpedWall; }
    bool blockedOf(s32 id) const { return m_enemies[static_cast<usize>(id)].blocked; }
    /** How far one of `kind` goes in a tick. */
    f32 paceOf(s32 kind) const;

private:
    enum class State : u8 { Inactive, Active, Asleep, Dying };

    struct Stock {
        s32 kind = -1;
        ItemArchive archive;
        TextureAnimator textures;
        std::array<const TreeInfo*, 3> trees{};
        std::array<TreeModel, 3> bodies;               ///< set to the frame before each is drawn
        std::array<const TreeInfo*, 3> variantTrees{}; ///< the archer's, bomber's, suicide's
        std::array<TreeModel, 3> variantBodies;
        TreeModel arrow;
        TreeModel bomb;
        TreeModel fireball; ///< the third slot's shot
        std::array<TreeModel, 2> deathStatues;
        std::array<const TreeInfo*, 2> deathStatueTrees{};
        std::array<BlobShadow, 3> shadows; ///< SHADOW1L1..3L1, by tier
        TreeInfo unseen; ///< the stance alone, for a kind with no body on the disc (IT)
    };

    struct Enemy {
        State state = State::Inactive;
        s32 kind = 0;
        s32 tier = 1;
        s32 algorithm = 0;
        s32 variant = 0; ///< the strength placed at: 4 an archer, 5 a bomber, 6 a suicide
        s32 generator = -1;
        bool bred = false; ///< a generator bred it
        f32 health = 0.0f;
        f32 fullHealth = 0.0f;
        f32 sight = kBaseSight;
        f32 radius = 1.0f;
        f32 height = 6.0f;
        f32 reach = 3.0f; ///< half the height: how high its body is struck
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 presentationPosition{0};
        f32 presentationYaw = 0;
        State presentationState = State::Inactive;
        struct Floor {
            s32 object = -1;
            Vec3 local{0};
        };
        std::optional<Floor> floor;
        f32 yaw = 0.0f; ///< the way the body faces
        MindMemory mind;
        Vec3 push{0.0f, 0.0f, 0.0f};
        f32 pushMagnitude = 0.0f;
        s32 pushes = 0;
        s32 target = -1;
        s32 targetBefore = -1;
        f32 targetDistance = 100000.0f;
        f32 weightedDistance = 100000.0f;
        bool recognized = false;
        s32 contact = -1;     ///< the player it is against
        s32 attackIndex = -1; ///< the player its swing is for
        s32 attackCount = 0;
        s32 stunTicks = 0;
        s32 drainTicks = 0;
        s32 endurance = 0;
        bool draining = false;
        bool bumpedWall = false;  ///< the last step ran into the world
        bool bumpedOther = false; ///< or another enemy
        bool blocked = false;     ///< and came to a dead stop
        s32 otherSide = 1;        ///< which way round the enemy it bumped is nearer
        bool expired = false;     ///< its mind is done with it
        f32 hurtPending = 0.0f;   ///< damage since the last reaction
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        s32 hurtBy = -1;
        bool killed = false;
        bool onScreen = true; ///< in view by a margin (visactive)
        s32 hitCount = 0;
        f32 veil = 0.0f;   ///< how far a veiling kind has faded out: 255 is gone from sight
        s32 veilClock = 0; ///< ticks it stays seen (above nought) or unseen (below)
        f32 flashSeconds = 0;
        f32 deathSeconds = 0;
        s32 deathSkinFrames = 0;
        std::string_view deathSkin;
        EnemyAnimator animator;
    };

    Stock* stockOf(s32 kind);
    const Stock* stockOf(s32 kind) const;
    std::optional<s32> takeSlot(const EnemySpawn& spawn);
    bool clearAt(Enemy& enemy, const Vec3& position, std::span<const EnemyView> players,
                 std::span<const Obstacle> obstacles, s32 self) const;
    void initialise(Enemy& enemy, const EnemySpawn& spawn, const EnemyKind& kind);
    void rememberFloor(Enemy& enemy) const;
    void touchHazards(Enemy& enemy, s32 slot);
    static void decayPush(Enemy& enemy, f32 seconds);
    f32 presentationBlend(const Enemy& enemy, f32 alpha) const;
    static void capturePresentation(Enemy& enemy);
    /** The warlock's coming and going: while it stands, walks or runs it stays seen a while,
     * fades out, stays unseen a while and fades back; doing anything else it shows. */
    void veil(Enemy& enemy, s32 ticks);
    void chooseTarget(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                      std::span<f32> crowding);
    void step(f32 seconds, std::span<const EnemyView> players, std::span<const Obstacle> obstacles,
              EnemyMissiles* missiles, f32 missileSpeedScale, bool timeStopped);
    /** Whether the swing it has just landed casts its missile rather than striking. */
    static bool castsNow(const Enemy& enemy);
    void resolveBlows(Enemy& enemy, s32 slot, std::span<const EnemyView> players);
    void drain(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players);
    void hurtDeath(Enemy& enemy, s32 slot, const EnemyHit& hit);
    static void react(Enemy& enemy);
    /** Sends the brood's death shot from the corpse at its player, else the first standing;
     * nobody standing, none goes (fn_8004F1DC). */
    void aimDeathShot(const Enemy& enemy, s32 slot, std::span<const EnemyView> players);
    MindSense sense(const Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
                    std::span<const Obstacle> obstacles) const;
    void think(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
               std::span<const Obstacle> obstacles);
    void shoot(Enemy& enemy, s32 slot, std::span<const EnemyView> players, EnemyMissiles& missiles,
               f32 speedScale, std::span<const Obstacle> items);
    struct Figure {
        TreeModel* model = nullptr;
        const TreeInfo* tree = nullptr;
        TextureAnimator* textures = nullptr;
    };
    Figure bodyOf(const Enemy& enemy);
    void move(Enemy& enemy, s32 slot, s32 ticks, f32 seconds, const Vec3& step,
              std::span<const EnemyView> players, std::span<const Obstacle> obstacles);
    Vec3 travel(const Enemy& enemy, const Vec3& from, const Vec3& to) const;
    std::optional<FloorHit> stepFloor(const Enemy& enemy, const Vec3& from, const Vec3& to) const;
    bool probeClear(const Enemy& enemy, const Vec3& at, std::span<const Obstacle> obstacles,
                    s32 self) const;
    static f32 turnToward(const Enemy& enemy, f32 wanted, s32 ticks);
    f32 fightOf(const Enemy& enemy) const;
    void die(Enemy& enemy);
    void detachGenerator(Enemy& enemy);
    static const EnemyView* viewOf(std::span<const EnemyView> players, s32 player);
    static f32 playerDistance(const Enemy& enemy, const EnemyView& player);
    static f32 wrap(f32 angle);
    static Vec3 bodyCentre(const Enemy& enemy);

    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    const HazardSurfaces* m_hazards = nullptr; ///< borrowed from the level
    LookoutRoute m_lookouts;
    s32 m_most = kMost;
    EnemyScales m_scales;
    std::vector<std::unique_ptr<Stock>> m_stocks;
    std::array<Enemy, kMost> m_enemies;
    std::vector<EnemyBlow> m_blows;
    std::vector<EnemyLoss> m_losses;
    std::vector<EnemyBurst> m_bursts;
    std::vector<EnemyCue> m_cues;
    std::vector<EnemyFeedback> m_feedback;
    std::vector<DeathEvent> m_deathEvents;
    std::vector<EnemyDeathShot> m_deathShots;
    std::vector<s32> m_tagged;
    std::vector<EnemyGeneratorEvent> m_generatorEvents;
    std::mt19937 m_random;
    s32 m_bomber = -1; ///< the lit suicide bomber the rest run from this tick
    std::optional<ViewVolume> m_view;
    s32 m_inView = 0;
    u32 m_frame = 0;
    s32 m_pendingTicks = 0;
    f32 m_pendingSeconds = 0;
    f32 m_shrink = 1.0f;
};

} // namespace gdl::game
