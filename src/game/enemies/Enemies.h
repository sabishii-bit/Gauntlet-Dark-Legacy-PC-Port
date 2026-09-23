#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"
#include "game/world/ItemFigure.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {

/** The level's scales on its enemies. */
struct EnemyScales {
    float health = 1.0f;
    float speed = 1.0f;
    float sight = 1.0f;
    float damage = 1.0f;
    float playerLevel = 0.0f; ///< the level the place is meant for; none when nought
};

/** A player as the enemies see one. */
struct EnemyView {
    std::int32_t player = -1;
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< its feet
    float radius = 1.0f;
    float height = 6.0f;
    std::int32_t level = 1;
    bool hidden = false; ///< not to be seen or sought
};

/** A blow an enemy has landed on a player. */
struct EnemyBlow {
    std::int32_t player = -1;
    std::int32_t kind = 0;
    std::int32_t tier = 1;
    float damage = 0.0f;
    bool power = false;               ///< the stronger every-eighth blow
    bool knocksDown = false;          ///< a tall one's power blow floors its victim
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< from the enemy to the player
};

/** An enemy hurt or killed by a player, and what that is worth. */
struct EnemyLoss {
    std::int32_t enemy = -1;
    std::int32_t kind = 0;
    std::int32_t tier = 1;
    std::int32_t player = -1;
    std::int32_t experience = 0;
    bool killed = false;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** What a player's hit carries besides damage: the original's damage-type bits that matter. */
struct EnemyHit {
    static constexpr std::uint32_t kKnockBack = 0x10;
    static constexpr std::uint32_t kKnockDown = 0x20;
    static constexpr std::uint32_t kFloors = 0x10160; ///< any of these throws the enemy down
    static constexpr std::uint32_t kMagic = 0x200;    ///< a magic hit over ten also throws it down

    float damage = 0.0f;
    std::uint32_t flags = 0;
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< the way the hit travels
    std::int32_t player = -1;         ///< who dealt it; -1 for the world
    std::int32_t level = 1;           ///< the character's level, against the place's
    std::optional<Vec3> where;        ///< where it landed, when that is known
    bool close = false;               ///< a blow struck in the hand, not a missile
};

/** Where an enemy is asked to appear: about `position`, facing `direction`. */
struct EnemySpawn {
    std::int32_t kind = kGruntKind;
    std::int32_t tier = 1;
    std::int32_t algorithm = -1; ///< the kind's own when negative
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f};
    float clearance = 0.0f; ///< how far out from `position` it is set (a generator's height)
    std::int32_t generator = -1;
    bool placed = false;          ///< set exactly where asked, as a level's placement is
    bool asleep = false;          ///< a placement of no strength waits to be woken
    std::int32_t idleTicks = 120; ///< a thrower's wait between throws
};

/** A blast one of the swarm goes up in. */
struct EnemyBurst {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float damage = 0.0f;
    std::int32_t enemy = -1;
};

/** The strengths a placement gives past the tiers: the variants. */
inline constexpr std::int32_t kArcherStrength = 4;
inline constexpr std::int32_t kBomberStrength = 5;
inline constexpr std::int32_t kSuicideStrength = 6;

/**
 * The swarm: the level's ordinary enemies, up to twenty-five of them, each with the mind the
 * original gives its kind. They see the nearest player within their sight, chase it hugging
 * the corners they bump into, and strike on touch, the blow landing as the swing ends whether
 * or not the player is still there. Struck, they flinch or are thrown down, and, killed, are
 * worth experience to the one who did it.
 */
class Enemies {
public:
    static constexpr std::int32_t kMost = 25;
    static constexpr float kBaseSight = 30.0f;
    static constexpr float kWallRadiusScale = 1.5f; ///< the body keeps this much off walls
    static constexpr float kMostPush = 40.0f;
    static constexpr float kPushDecay = 0.8f;
    static constexpr float kBlowGrowth = 1.5f;      ///< a power blow's share over an ordinary one
    static constexpr float kKnockDownHeight = 2.0f; ///< a body reaching above this floors
    static constexpr float kSuicideDamage = 50.0f;  ///< at the level's enemy damage
    static constexpr std::int32_t kTicksPerSecond = 60;

    Enemies() = default;
    Enemies(const Enemies&) = delete;
    Enemies& operator=(const Enemies&) = delete;
    Enemies(Enemies&&) = delete;
    Enemies& operator=(Enemies&&) = delete;
    ~Enemies();

    /** Prepares to hold the level's enemies: their kinds' archives are loaded from
     * `unpackedRoot` as they are first asked for. */
    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, std::int32_t most, const EnemyScales& scales,
              std::uint32_t seed);
    void close();

    /** Loads a kind's archive ahead of need; false when it is not there. */
    bool loadKind(std::int32_t kind);
    bool kindLoaded(std::int32_t kind) const;
    const ItemArchive* archiveOf(std::int32_t kind) const;
    ItemArchive* archive(std::int32_t kind);
    /** The prefix a kind's bodies are named by ("GRU") and the tree of a tier ("GRU1"). */
    const TreeInfo* treeOf(std::int32_t kind, std::int32_t tier) const;

    /** Puts one where asked, the way the original does: for a generator's, in one of the
     * clear octants about it; a placement's exactly there. Nullopt when there is no room or
     * no slot worth taking. */
    std::optional<std::int32_t> spawn(const EnemySpawn& spawn, std::span<const EnemyView> players,
                                      std::span<const Obstacle> obstacles = {});
    /** Wakes a sleeping placement. */
    void wake(std::int32_t id);
    /** Forgets everything of a generator that is gone. */
    void generatorGone(std::int32_t generator);

    /** Steps every mind and body `ticks` (`seconds` long) with the players where they are;
     * what the throwers let go flies in `missiles`, when given. */
    void update(std::int32_t ticks, float seconds, std::span<const EnemyView> players,
                std::span<const Obstacle> obstacles = {}, class EnemyMissiles* missiles = nullptr,
                float missileSpeedScale = 1.0f);
    std::vector<EnemyBlow> takeBlows();
    std::vector<EnemyLoss> takeLosses();
    std::vector<EnemyBurst> takeBursts();

    /** Deals a hit to an enemy; what it is worth comes back as a loss. */
    void hurt(std::int32_t id, const EnemyHit& hit);
    /** The enemies a missile can strike. */
    std::vector<MissileTarget> targets() const;
    /** The nearest live enemy whose body a blow sweeping from `from` to `to` with `radius`
     * touches. */
    std::optional<std::int32_t> struckBy(const Vec3& from, const Vec3& to, float radius) const;
    /** The live enemies within `radius` of `centre`. */
    std::vector<std::int32_t> within(const Vec3& centre, float radius) const;
    /** The live enemies a strike reaches. */
    std::vector<std::int32_t> reachedBy(const Vec3& centre, float radius, float arc,
                                        const Vec3& facing) const;

    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting);

    bool alive(std::int32_t id) const;
    bool dying(std::int32_t id) const;
    std::size_t count() const; ///< live, dying included
    std::int32_t kindOf(std::int32_t id) const;
    std::int32_t tierOf(std::int32_t id) const;
    std::int32_t generatorOf(std::int32_t id) const;
    float healthOf(std::int32_t id) const;
    const Vec3& positionOf(std::int32_t id) const;
    float yawOf(std::int32_t id) const;
    float radiusOf(std::int32_t id) const;
    float heightOf(std::int32_t id) const;
    std::int32_t targetOf(std::int32_t id) const;
    std::int32_t algorithmOf(std::int32_t id) const;
    std::int32_t pushCountOf(std::int32_t id) const;
    std::int32_t variantOf(
        std::int32_t id) const; ///< the strength it was placed at, four and over for a variant
    const EnemyAnimator* animatorOf(std::int32_t id) const;
    const MindMemory& memoryOf(std::int32_t id) const {
        return m_enemies[static_cast<std::size_t>(id)].mind;
    }
    bool bumpedWallOf(std::int32_t id) const {
        return m_enemies[static_cast<std::size_t>(id)].bumpedWall;
    }
    bool blockedOf(std::int32_t id) const {
        return m_enemies[static_cast<std::size_t>(id)].blocked;
    }
    /** How far one of `kind` goes in a tick. */
    float paceOf(std::int32_t kind) const;

private:
    enum class State : std::uint8_t { Inactive, Active, Asleep, Dying };

    struct Stock {
        std::int32_t kind = -1;
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
        std::int32_t kind = 0;
        std::int32_t tier = 1;
        std::int32_t algorithm = 0;
        std::int32_t variant = 0; ///< the strength placed at: 4 an archer, 5 a bomber, 6 a suicide
        std::int32_t idleTicks = 120;
        bool threw = false;
        std::int32_t generator = -1;
        float health = 0.0f;
        float fullHealth = 0.0f;
        float sight = kBaseSight;
        float radius = 1.0f;
        float height = 6.0f;
        float reach = 3.0f; ///< half the height: how high its body is struck
        Vec3 position{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f; ///< the way the body faces
        MindMemory mind;
        Vec3 push{0.0f, 0.0f, 0.0f};
        float pushMagnitude = 0.0f;
        std::int32_t pushes = 0;
        std::int32_t target = -1;
        std::int32_t targetBefore = -1;
        float targetDistance = 100000.0f;
        float weightedDistance = 100000.0f;
        bool recognized = false;
        std::int32_t contact = -1;     ///< the player it is against
        std::int32_t attackIndex = -1; ///< the player its swing is for
        std::int32_t attackCount = 0;
        std::int32_t stunTicks = 0;
        bool bumpedWall = false;    ///< the last step ran into the world
        bool bumpedOther = false;   ///< or another enemy
        bool blocked = false;       ///< and came to a dead stop
        std::int32_t otherSide = 1; ///< which way round the enemy it bumped is nearer
        bool expired = false;       ///< its mind is done with it
        float hurtPending = 0.0f;   ///< damage since the last reaction
        std::uint32_t hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        std::int32_t hurtBy = -1;
        bool killed = false;
        EnemyAnimator animator;
    };

    Stock* stockOf(std::int32_t kind);
    const Stock* stockOf(std::int32_t kind) const;
    std::optional<std::int32_t> takeSlot(const EnemySpawn& spawn,
                                         std::span<const EnemyView> players);
    bool clearAt(Enemy& enemy, const Vec3& position, std::span<const EnemyView> players,
                 std::span<const Obstacle> obstacles, std::int32_t self) const;
    void initialise(Enemy& enemy, const EnemySpawn& spawn, const EnemyKind& kind);
    void chooseTarget(Enemy& enemy, std::int32_t slot, std::span<const EnemyView> players,
                      std::span<float> crowding);
    void resolveBlows(Enemy& enemy, std::int32_t slot, std::span<const EnemyView> players);
    static void react(Enemy& enemy);
    MindSense sense(const Enemy& enemy, std::int32_t slot, std::int32_t ticks,
                    std::span<const EnemyView> players, std::span<const Obstacle> obstacles) const;
    void think(Enemy& enemy, std::int32_t slot, std::int32_t ticks,
               std::span<const EnemyView> players, std::span<const Obstacle> obstacles);
    void shoot(Enemy& enemy, std::int32_t slot, std::span<const EnemyView> players,
               EnemyMissiles& missiles, float speedScale);
    const TreeModel* bodyOf(const Enemy& enemy);
    void move(Enemy& enemy, std::int32_t slot, std::int32_t ticks, float seconds, const Vec3& step,
              std::span<const EnemyView> players, std::span<const Obstacle> obstacles);
    bool probeClear(const Enemy& enemy, const Vec3& at, std::span<const Obstacle> obstacles,
                    std::int32_t self) const;
    static float turnToward(const Enemy& enemy, float wanted, std::int32_t ticks);
    float fightOf(const Enemy& enemy) const;
    static void die(Enemy& enemy);
    static const EnemyView* viewOf(std::span<const EnemyView> players, std::int32_t player);
    static float wrap(float angle);
    static Vec3 bodyCentre(const Enemy& enemy);

    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    std::int32_t m_most = kMost;
    EnemyScales m_scales;
    std::vector<std::unique_ptr<Stock>> m_stocks;
    std::array<Enemy, kMost> m_enemies;
    std::vector<EnemyBlow> m_blows;
    std::vector<EnemyLoss> m_losses;
    std::vector<EnemyBurst> m_bursts;
    std::mt19937 m_random;
    std::uint32_t m_frame = 0;
};

} // namespace gdl::game
