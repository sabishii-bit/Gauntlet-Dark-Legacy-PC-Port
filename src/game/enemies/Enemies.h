#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/math/Mt19937.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/combat/MeleeGeometry.h"
#include "game/effects/BlobShadow.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"
#include "game/enemies/SwarmAggression.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/ItemFigure.h"
#include "game/party/PlayerMissiles.h"

namespace gdl::game {

class NativeStateResources;

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
    bool operator==(const EnemyScales&) const = default;
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
    std::optional<f32> collisionRadius = std::nullopt; ///< native contact radius, else radius
    s32 heldDeath = -1; ///< current Anti-Death contact, refreshed with the player's state
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
    bool operator==(const EnemyBlow&) const = default;
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
    s32 element = 0; ///< an elemental one's element, 0 for a plain body
    bool operator==(const EnemyLoss&) const = default;
};

/** A living swarm member's collision body, independent of its render/aiming bounds. */
struct EnemyBody {
    s32 id = -1;
    Vec3 centre{0};
    f32 radius = 0;
    f32 halfHeight = 0;
    bool operator==(const EnemyBody&) const = default;
};

/** Ordered generator bookkeeping, including patrol offspring's change to future births. */
struct EnemyGeneratorEvent {
    enum class Kind : u8 { Born, Detached, PatrolHit, PatrolDetached };
    s32 generator = -1;
    Kind kind = Kind::Born;
    bool operator==(const EnemyGeneratorEvent&) const = default;
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
    f32 clearance = 0.0f; ///< how far out from `position` it is set (a generator's radius)
    s32 generator = -1;
    bool placed = false;             ///< set exactly where asked, as a level's placement is
    bool patrolBirth = false;        ///< generate_single's sentry, not an ordinary repeating brood
    bool allBirthDirections = false; ///< special -2/-3 generators bypass the resolved species mask
    bool asleep = false;             ///< a placement of no strength waits to be woken
    f32 throwInterval = 1.0f;        ///< seconds, before the level's missile-rate scale
    Priority priority = Priority::Offscreen; ///< replacement permission, independent of strength
    f32 elementalChance = 0.0f; ///< the odds a swarm birth is an elemental one (Aggressor Mode)
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
    bool operator==(const EnemyDeathShot&) const = default;
};

/** A blast one of the swarm goes up in. */
struct EnemyBurst {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    s32 enemy = -1;
    s32 kind = -1;             ///< whose fragments scatter when it explodes
    bool silencesYell = false; ///< set off by a hit rather than its own fuse
    u64 instance = 0;          ///< retained after the bomber's body disappears
    bool operator==(const EnemyBurst&) const = default;
};

/** One of the swarm appearing: a brood at its generator's spot, a placement at its own. */
struct EnemyBirth {
    Vec3 position{0.0f, 0.0f, 0.0f};
    s32 kind = -1;
    s32 tier = 1; ///< one to three; the variants placed at a strength over three are not reported
    bool operator==(const EnemyBirth&) const = default;
};

/** A sound one of the swarm makes other than on being hit: a throw going off (the arrow's
 * or a bolt's), or a suicide's yell as it starts its run. */
struct EnemyCue {
    enum class Kind : u8 { Arrow, Bolt, Yell };
    Kind kind = Kind::Arrow;
    s32 enemyKind = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    /** Captured at emission: the pool slot may be recycled before this is consumed. */
    s32 enemy = -1;
    u64 instance = 0;
    bool operator==(const EnemyCue&) const = default;
};

void visitNativeState(NativeStateDigest& digest, const EnemyScales& value);
void visitNativeState(NativeStateDigest& digest, const EnemyBlow& value);
void visitNativeState(NativeStateDigest& digest, const EnemyLoss& value);
void visitNativeState(NativeStateDigest& digest, const EnemyBody& value);
void visitNativeState(NativeStateDigest& digest, const EnemyGeneratorEvent& value);
void visitNativeState(NativeStateDigest& digest, const EnemySpawn& value);
void visitNativeState(NativeStateDigest& digest, const EnemyDeathShot& value);
void visitNativeState(NativeStateDigest& digest, const EnemyBurst& value);
void visitNativeState(NativeStateDigest& digest, const EnemyBirth& value);
void visitNativeState(NativeStateDigest& digest, const EnemyCue& value);

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
    /** The pool: the original's twenty-five and room past it for a frenzied swarm; a level
     * opens with its own cap, `widenPool` lifts it to the whole. */
    static constexpr s32 kMost = 64;
    static constexpr f32 kBaseSight = 30.0f;
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
    void setView(std::optional<ViewVolume> view) {
        m_simulation.view = view ? std::optional{ViewSet::of(*view, Vec3{0.0f})} : std::nullopt;
    }
    /** What the party's cameras take in together, under solo cameras. */
    void setViews(const std::optional<ViewSet>& views) { m_simulation.view = views; }
    /** The swarm's aggression for the coming step, births and drawing: quicker, tougher,
     * and past the bravery line red with an aura. Set before each step; not kept. */
    void setAggression(const SwarmAggression& aggression) { m_aggression = aggression; }
    const SwarmAggression& aggression() const { return m_aggression; }
    /** The bodies the last draw marked with the rage's aura, for the log. */
    s32 auraBodies() const { return m_auraBodies; }
    /** The scale the party's enemy shrinkers hold the swarm at (`EnemyShrink`): shown and
     * shadowed at it, taking double and dealing half while under one. */
    void setShrink(f32 scale) { m_simulation.shrink = scale; }
    f32 shrink() const { return m_simulation.shrink; }
    /** How many stand in view by twice their radius, as of the last update (do_enemies);
     * without a view, all of them. */
    s32 inView() const { return m_simulation.inView; }
    /** The lookouts its patrollers walk between. */
    void setLookouts(LookoutRoute lookouts) { m_simulation.lookouts = std::move(lookouts); }
    /** Snapshot of the larger creatures' solid collision nodes and root fallbacks.
     * Refreshed before births and movement; owns the values, not the caller's view. */
    void setCombatantBodies(std::span<const MissileTarget> bodies);
    /** World bounds used by native contact ties: z cells, then x cells, then
     * descending enemy slots. Unconfigured/degenerate bounds use one cell. */
    void setContactGridBounds(const Vec3& minimum, const Vec3& maximum);

    /** Loads a kind's archive ahead of need; false when it is not there. */
    bool loadKind(s32 kind);
    /** Optional immutable resource provider, installed before loading any kind.
     * A supplied provider owns the whole roster; missing art is not substituted. */
    using ArchiveLoader = std::function<std::unique_ptr<ItemArchive>(s32)>;
    bool setArchiveLoader(ArchiveLoader loader);
    bool kindLoaded(s32 kind) const;
    const ItemArchive* archiveOf(s32 kind) const;
    ItemArchive* archive(s32 kind);
    /** Retained stage artwork includes kinds admitted only by a discarded native branch. */
    bool registerNativeResources(NativeStateResources& resources,
                                 std::string_view stablePrefix) const;
    /** Already-loaded arrow/bomb/bolt model; never loads assets or starts a shot. */
    const TreeModel* projectileModel(s32 kind, s32 slot) const;
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
    /** The ordinary births since last asked (generate_enemy's, short of the variants). */
    std::vector<EnemyBirth> takeBirths();
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
    /** Native acquisition order, lifetime identity and dynamic-grid contact tie
     * keys. IDs remain pool-local until a scene composes its target roster. */
    std::vector<MeleeGeometry::Target> meleeGeometry() const;
    /** PlayerCollideItems' swarm contact, separate from missile/physical movement reach.
     * Positions are collision centres; width is the player's full native col_radius.
     * Returns the contact nearest the step endpoint, not the nearest enemy centre.
     * On success, optionally writes its horizontal distance from that endpoint. */
    std::optional<s32> playerContact(const Vec3& from, const Vec3& to, f32 width, f32 halfHeight,
                                     f32* endpointDistance = nullptr) const;
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
              const CameraFrame* camera = nullptr, f32 presentationAlpha = -1,
              TreeModel::Pass pass = TreeModel::Pass::All);

    /** Display-only grounded interpolation. Optional endpoint floor copies let
     * replicas prove contact against the corresponding host geometry clocks.
     * Does not mutate bodies, floors, animation, damage, or snapshot positions. */
    static Vec3 supportedPresentation(const WorldCollision* collision, const Vec3& from,
                                      const Vec3& to, f32 blend, EnemyAction action, bool active,
                                      const WorldCollision* fromFloor = nullptr,
                                      const WorldCollision* toFloor = nullptr);

    bool alive(s32 id) const;
    bool dying(s32 id) const;
    /** Marks `id` out with `highlight` as it is drawn, as the developer console's
     * selection; null, or nobody's id, marks nothing. */
    void setHighlight(s32 id, const SelectionHighlight* highlight) {
        m_highlightedId = id;
        m_highlight = highlight;
    }
    /** Read-only lifetime/health observation. Pool slots can be recycled without
     * an intervening empty frame; instance is never reused by this owner, even
     * across close/open. Does not expose AI mutation or consume feedback queues. */
    struct Observation {
        u64 instance = 0;
        f32 health = 0;
        f32 fullHealth = 0;
        bool asleep = false;
        bool hitFlash = false;
        f32 alpha = 1;
        f32 scale = 1;
        u32 deathSkin = 0;
        u32 deathFrame = 0;
    };
    std::optional<Observation> observe(s32 id) const;
    /** Native current-roster draw state, including the swarm's two-tick clock.
     * Pose artwork is borrowed from the already loaded kind archive. */
    struct Presentation {
        u64 instance = 0;
        s32 kind = 0, tier = 1, variant = 0;
        Vec3 previousPosition{0}, position{0};
        f32 previousYaw = 0, yaw = 0;
        f32 blendStart = 1, blendEnd = 1;
        f32 textureStart = 0, textureEnd = 0;
        EnemyAnimator::Presentation animation;
    };
    std::optional<Presentation> presentation(s32 id) const;
    /** Resource selection, independent of a live enemy slot. */
    struct Appearance {
        s32 kind = 0;
        s32 tier = 1;
        s32 variant = 0;
        bool asleep = false;
    };
    const TreeInfo* appearanceTree(const Appearance& appearance);
    static constexpr std::array<std::string_view, 6> kDeathSkins{
        "DEATHBLOOD", "DEATHFIRE", "DEATHELEC", "DEATHLIGHT", "DEATHACID", "DEATHALT"};
    /** Draw only an externally posed body using already-loaded resources. Does
     * not create a collision body, run AI, emit rewards or advance animation.
     * Shadows and one-shot effects are separate from this base pass. */
    void drawPose(RenderDevice& device, const Mat4& clip, const Mat4& placement,
                  const WorldLighting& lighting, const Appearance& appearance, const TreePose& pose,
                  u32 sequence, f32 frame, f32 textureFrame, const Texture* hitFlash,
                  const CameraFrame* camera, TreeModel::Pass pass = TreeModel::Pass::All,
                  f32 alpha = 1, const Texture* deathSkin = nullptr, bool dying = false);
    usize count() const; ///< live, dying included
    s32 most() const { return m_simulation.most; } ///< the slots in play, the level's cap
    /** Lifts the cap to the whole pool while the swarm is frenzied. */
    void widenPool() { m_simulation.most = kMost; }
    /** Brings the cap back toward the level's own, as far as the bodies past it allow: the
     * frenzy over, the swarm thins back to what the level allows as its extras fall. */
    void narrowPool();
    s32 levelCap() const { return m_levelCap; }
    s32 kindOf(s32 id) const;
    s32 tierOf(s32 id) const;
    s32 generatorOf(s32 id) const;
    /** Whether a generator bred it, even one since gone (the original's birth_style 0). */
    bool bred(s32 id) const;
    f32 healthOf(s32 id) const;
    const Vec3& positionOf(s32 id) const;
    /** Actual retained support, including dying bodies; -1 for absent/unattached slots. */
    s32 floorOf(s32 id) const;
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
    s32 elementOf(s32 id) const;   ///< an elemental one's element, 0 for a plain body
    u64 instanceOf(s32 id) const;  ///< the lifetime the slot holds, 0 when none
    /** Aggressor Mode's elemental one: born to a frenzied hut by chance, three and a half
     * times as tough and half again as big, immune to its own element, dealing it. */
    static constexpr f32 kElementalHealth = 3.5f;
    static constexpr f32 kElementalSize = 1.5f;
    struct BodyState;
    static f32 sizeOf(const BodyState& body);
    f32 sightOf(s32 id) const;     ///< how far it sees, at the level's scale
    s32 stunTicksOf(s32 id) const; ///< ticks it still stands still after appearing
    /** How solid it shows: one, or less while a veiling kind fades. */
    f32 opacityOf(s32 id) const {
        return 1.0f - m_simulation.enemies[static_cast<usize>(id)].veil / kVeiled;
    }
    static constexpr s32 kVeilingKind = 24; ///< the warlock (fn_8004D958's type 24)
    static constexpr f32 kVeiled = 255.0f;
    const EnemyAnimator* animatorOf(s32 id) const;
    const MindMemory& memoryOf(s32 id) const {
        return m_simulation.enemies[static_cast<usize>(id)].mind;
    }
    bool bumpedWallOf(s32 id) const {
        return m_simulation.enemies[static_cast<usize>(id)].bumpedWall;
    }
    bool blockedOf(s32 id) const { return m_simulation.enemies[static_cast<usize>(id)].blocked; }
    /** How far one of `kind` goes in a tick. */
    f32 paceOf(s32 kind) const;

    enum class State : u8 { Inactive, Active, Asleep, Dying };
    enum class Birth : u8 { Brood, Placement, Patrol };

    /** Pointer-free native slot state shared with checkpoints, so a new AI,
     * contact or lifecycle field cannot silently disappear from capture. */
    struct BodyState {
        u64 instance = 0;
        State state = State::Inactive;
        s32 kind = 0;
        s32 tier = 1;
        s32 algorithm = 0;
        s32 variant = 0; ///< the strength placed at: 4 an archer, 5 a bomber, 6 a suicide
        s32 element = 0; ///< Aggressor Mode's elemental one: 1 fire, 2 electric, 3 light, 4 acid
        s32 generator = -1;
        bool bred = false; ///< a generator bred it
        Birth birth = Birth::Brood;
        f32 health = 0.0f;
        f32 fullHealth = 0.0f;
        f32 sight = kBaseSight;
        f32 radius = 1.0f;
        f32 height = 6.0f;
        f32 reach = 3.0f; ///< half the height: how high its body is struck
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 birthPosition{0}; ///< native floor-delta rule's fixed spawn reference
        Vec3 presentationPosition{0};
        f32 presentationYaw = 0;
        State presentationState = State::Inactive;
        struct Floor {
            s32 object = -1;
            Vec3 local{0};
            bool operator==(const Floor&) const = default;
            friend void visitNativeState(NativeStateDigest& digest, const Floor& value);
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
        f32 burnGap = 0;
        f32 deathSeconds = 0;
        s32 deathSkinFrames = 0;
        std::string deathSkin;
        bool valid() const;
        bool operator==(const BodyState&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const BodyState& value);
    };

    struct Checkpoint {
        static constexpr usize kMaxEvents = 65536;
        static constexpr usize kMaxCombatants = 4096;
        struct Body : BodyState {
            std::optional<EnemyAnimator::Checkpoint> animator;
            bool operator==(const Body&) const = default;
        };
        struct Stock {
            s32 kind = -1;
            TextureAnimator::Checkpoint textures;
            bool operator==(const Stock&) const = default;
        };
        std::array<Body, kMost> enemies;
        std::vector<Stock> stocks;
        u64 nextInstance = 1;
        Mt19937::Checkpoint random = Mt19937{}.checkpoint();
        s32 most = kMost;
        EnemyScales scales;
        LookoutRoute lookouts;
        std::vector<EnemyBlow> blows;
        std::vector<EnemyLoss> losses;
        std::vector<EnemyBurst> bursts;
        std::vector<EnemyBirth> births;
        std::vector<EnemyCue> cues;
        std::vector<EnemyFeedback> feedback;
        std::vector<DeathEvent> deathEvents;
        std::vector<EnemyDeathShot> deathShots;
        std::vector<s32> tagged;
        std::vector<EnemyGeneratorEvent> generatorEvents;
        std::vector<EnemyBody> combatants;
        Vec2 contactGridOrigin{0};
        f32 contactGridInverseWidth = 0;
        s32 bomber = -1;
        std::optional<ViewSet> view;
        s32 inView = 0;
        u32 frame = 0;
        s32 pendingTicks = 0;
        f32 pendingSeconds = 0;
        f32 shrink = 1;
        bool valid() const;
        bool operator==(const Checkpoint& /*other*/) const;
    };
    std::optional<Checkpoint> checkpoint() const;
    /** Prepared against this owner's loaded resources. No live state is changed
     * until commit; callers can prepare several dependent pools atomically. */
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class Enemies;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    std::optional<Prepared> prepareRestore(const Checkpoint& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation-free commit. Prepare/preflight/commit must run on the same
     * simulation thread without intervening gameplay or resource mutation.
     * A successful commit consumes its token and invalidates sibling prepares. */
    bool commitRestore(Prepared&& prepared);
    /** Exact, atomic restore against the same opened world's loaded stock roster.
     * Never loads assets, spawns actors or dispatches queued world events. Rewinds
     * birth counters too: callers must quarantine replay and reconcile published
     * lifetimes/events before using a restored pool as live authority. External
     * collision, hazards, generators and missiles require their own matching state. */
    bool restore(const Checkpoint& state);

private:
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

    struct Enemy : BodyState {
        EnemyAnimator animator;
        bool operator==(const Enemy&) const = default;
    };

    Stock* stockOf(s32 kind);
    const Stock* stockOf(s32 kind) const;
    std::optional<s32> takeSlot(const EnemySpawn& spawn);
    bool birthPathClear(const Enemy& enemy, const Vec3& from, const Vec3& to,
                        std::span<const EnemyView> players, s32 self) const;
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
    Figure bodyOf(const Appearance& appearance);
    SwarmAggression m_aggression;                    ///< this step's, from the simulation
    s32 m_auraBodies = 0;                            ///< of the last draw, for the log
    s32 m_highlightedId = -1;                        ///< the developer console's selection
    const SelectionHighlight* m_highlight = nullptr; ///< how it is marked out, while drawn
    void move(Enemy& enemy, s32 slot, s32 ticks, f32 seconds, const Vec3& step,
              std::span<const EnemyView> players, std::span<const Obstacle> obstacles,
              bool seeking = false);
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
    MeleeGeometry::Target meleeBody(s32 id) const;

    friend class OpponentCombatCheckpoint;
    RenderDevice* m_device = nullptr;
    s32 m_levelCap = kMost; ///< the cap the level opened with, never simulation state
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_restoreGeneration = 0;
    std::filesystem::path m_root;
    ArchiveLoader m_archiveLoader;
    const WorldCollision* m_collision = nullptr;
    const HazardSurfaces* m_hazards = nullptr; ///< borrowed from the level
    // Live mutable values, copied directly for same-process rewind. Enemy animators
    // borrow immutable trees from m_stockCache; poses, RNG and event queues are values.
    struct Simulation {
        LookoutRoute lookouts;
        s32 most = kMost;
        EnemyScales scales;
        std::vector<Stock*> stocks;
        std::array<Enemy, kMost> enemies;
        u64 nextInstance = 1;
        std::vector<EnemyBlow> blows;
        std::vector<EnemyLoss> losses;
        std::vector<EnemyBurst> bursts;
        std::vector<EnemyBirth> births;
        std::vector<EnemyCue> cues;
        std::vector<EnemyFeedback> feedback;
        std::vector<DeathEvent> deathEvents;
        std::vector<EnemyDeathShot> deathShots;
        std::vector<s32> tagged;
        std::vector<EnemyGeneratorEvent> generatorEvents;
        std::vector<EnemyBody> combatants;
        Vec2 contactGridOrigin{0};
        f32 contactGridInverseWidth = 0;
        Mt19937 random;
        s32 bomber = -1; ///< the lit suicide bomber the rest run from this tick
        std::optional<ViewSet> view;
        s32 inView = 0;
        u32 frame = 0;
        s32 pendingTicks = 0;
        f32 pendingSeconds = 0;
        f32 shrink = 1.0f;
    };

public:
    /** Reusable local memory only. No wire representation or resource-rebinding protocol.
     * The opened pool owns all borrowed artwork; close/open invalidates the snapshot. */
    class LocalSnapshot {
    public:
        LocalSnapshot() = default;

    private:
        friend class Enemies;
        Simulation m_simulation;
        std::vector<TextureAnimator::Clock> m_textureClocks;
        std::shared_ptr<const u8> m_owner;
        u64 m_epoch = 0;
        const WorldCollision* m_collision = nullptr;
        const HazardSurfaces* m_hazards = nullptr;
    };
    bool saveLocal(LocalSnapshot& destination) const;
    bool canRestoreLocal(const LocalSnapshot& source) const;
    /** Exact same-owner native values, including inactive slots, RNG, pending
     * outputs and retained animation. Shared art/draw caches are not compared. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    /** Complete captured native pool values, not its public presentation. Immutable
     * art is resolved through stage resource identities; draw caches are excluded. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    /** Restores values in place, retaining archives and GPU resources. May reuse this
     * snapshot repeatedly; collision/hazard state is owned by the enclosing scene. */
    bool restoreLocal(const LocalSnapshot& source);

private:
    static void appendSimulationDigest(
        NativeStateDigest& digest, const Simulation& state,
        std::optional<std::span<const TextureAnimator::Clock>> savedClocks = std::nullopt);
    Simulation m_simulation;
    // Retain lazily loaded archives until close, even when a rewind hides their kind.
    // A replayed loadKind reactivates the same resource without disk/GPU work.
    std::vector<std::unique_ptr<Stock>> m_stockCache;
    u64 m_resourceEpoch = 0;
};

} // namespace gdl::game
