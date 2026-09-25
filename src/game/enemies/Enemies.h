#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"
#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {

/** The level's scales on its enemies. */
struct EnemyScales {
    f32 health = 1.0f;
    f32 speed = 1.0f;
    f32 sight = 1.0f;
    f32 damage = 1.0f;
    f32 playerLevel = 0.0f;     ///< the level the place is meant for; none when nought
    bool bossEncounter = false; ///< applies to every opponent in the arena, not just the boss
};

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
};

/** Where an enemy is asked to appear: about `position`, facing `direction`. */
struct EnemySpawn {
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
    s32 idleTicks = 120;                     ///< a thrower's wait between throws
    Priority priority = Priority::Offscreen; ///< replacement permission, independent of strength
};

/** A blast one of the swarm goes up in. */
struct EnemyBurst {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    s32 enemy = -1;
};

/** The strengths a placement gives past the tiers: the variants. */
inline constexpr s32 kArcherStrength = 4;
inline constexpr s32 kBomberStrength = 5;
inline constexpr s32 kSuicideStrength = 6;

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

    /** Steps every mind and body `ticks` (`seconds` long) with the players where they are;
     * what the throwers let go flies in `missiles`, when given. */
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {}, class EnemyMissiles* missiles = nullptr,
                f32 missileSpeedScale = 1.0f);
    std::vector<EnemyBlow> takeBlows();
    std::vector<EnemyLoss> takeLosses();
    std::vector<EnemyBurst> takeBursts();
    std::vector<EnemyFeedback> takeFeedback();

    /** Deals a hit to an enemy; what it is worth comes back as a loss. */
    void hurt(s32 id, const EnemyHit& hit);
    /** The enemies a missile can strike. */
    std::vector<MissileTarget> targets() const;
    /** The nearest live enemy whose body a blow sweeping from `from` to `to` with `radius`
     * touches. */
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    /** The live enemies within `radius` of `centre`. */
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    /** The live enemies a strike reaches. */
    std::vector<s32> reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* hitFlash = nullptr, ItemArchive* weapons = nullptr);

    bool alive(s32 id) const;
    bool dying(s32 id) const;
    usize count() const; ///< live, dying included
    s32 kindOf(s32 id) const;
    s32 tierOf(s32 id) const;
    s32 generatorOf(s32 id) const;
    f32 healthOf(s32 id) const;
    const Vec3& positionOf(s32 id) const;
    f32 yawOf(s32 id) const;
    f32 radiusOf(s32 id) const;
    f32 heightOf(s32 id) const;
    s32 targetOf(s32 id) const;
    s32 algorithmOf(s32 id) const;
    s32 pushCountOf(s32 id) const;
    s32 variantOf(s32 id) const; ///< the strength it was placed at, four and over for a variant
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
        std::array<const TreeInfo*, 3> trees{};
        std::array<TreeModel, 3> bodies;               ///< set to the frame before each is drawn
        std::array<const TreeInfo*, 3> variantTrees{}; ///< the archer's, bomber's, suicide's
        std::array<TreeModel, 3> variantBodies;
        TreeModel arrow;
        TreeModel bomb;
    };

    struct Enemy {
        State state = State::Inactive;
        s32 kind = 0;
        s32 tier = 1;
        s32 algorithm = 0;
        s32 variant = 0; ///< the strength placed at: 4 an archer, 5 a bomber, 6 a suicide
        s32 idleTicks = 120;
        bool threw = false;
        s32 generator = -1;
        f32 health = 0.0f;
        f32 fullHealth = 0.0f;
        f32 sight = kBaseSight;
        f32 radius = 1.0f;
        f32 height = 6.0f;
        f32 reach = 3.0f; ///< half the height: how high its body is struck
        Vec3 position{0.0f, 0.0f, 0.0f};
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
        s32 hitCount = 0;
        f32 flashSeconds = 0;
        f32 deathSeconds = 0;
        s32 deathSkinFrames = 0;
        std::string_view deathSkin;
        EnemyAnimator animator;
    };

    Stock* stockOf(s32 kind);
    const Stock* stockOf(s32 kind) const;
    std::optional<s32> takeSlot(const EnemySpawn& spawn, std::span<const EnemyView> players);
    bool clearAt(Enemy& enemy, const Vec3& position, std::span<const EnemyView> players,
                 std::span<const Obstacle> obstacles, s32 self) const;
    void initialise(Enemy& enemy, const EnemySpawn& spawn, const EnemyKind& kind);
    void chooseTarget(Enemy& enemy, s32 slot, std::span<const EnemyView> players,
                      std::span<f32> crowding);
    void resolveBlows(Enemy& enemy, s32 slot, std::span<const EnemyView> players);
    static void react(Enemy& enemy);
    MindSense sense(const Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
                    std::span<const Obstacle> obstacles) const;
    void think(Enemy& enemy, s32 slot, s32 ticks, std::span<const EnemyView> players,
               std::span<const Obstacle> obstacles);
    void shoot(Enemy& enemy, s32 slot, std::span<const EnemyView> players, EnemyMissiles& missiles,
               f32 speedScale);
    const TreeModel* bodyOf(const Enemy& enemy);
    void move(Enemy& enemy, s32 slot, s32 ticks, f32 seconds, const Vec3& step,
              std::span<const EnemyView> players, std::span<const Obstacle> obstacles);
    bool probeClear(const Enemy& enemy, const Vec3& at, std::span<const Obstacle> obstacles,
                    s32 self) const;
    static f32 turnToward(const Enemy& enemy, f32 wanted, s32 ticks);
    f32 fightOf(const Enemy& enemy) const;
    static void die(Enemy& enemy);
    static const EnemyView* viewOf(std::span<const EnemyView> players, s32 player);
    static f32 wrap(f32 angle);
    static Vec3 bodyCentre(const Enemy& enemy);

    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    s32 m_most = kMost;
    EnemyScales m_scales;
    std::vector<std::unique_ptr<Stock>> m_stocks;
    std::array<Enemy, kMost> m_enemies;
    std::vector<EnemyBlow> m_blows;
    std::vector<EnemyLoss> m_losses;
    std::vector<EnemyBurst> m_bursts;
    std::vector<EnemyFeedback> m_feedback;
    std::mt19937 m_random;
    u32 m_frame = 0;
};

} // namespace gdl::game
