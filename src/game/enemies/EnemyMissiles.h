#pragma once

#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** What the original's table says of a kind's missile: its harm, pace, size and flight. */
struct EnemyMissileKind {
    static constexpr s32 kArrow = 0; ///< the slot a shot takes
    static constexpr s32 kBomb = 1;  ///< and a lob
    static constexpr s32 kBolt = 2;  ///< and a kind's own shot
    static constexpr u32 kKnockBack = 0x10;
    static constexpr u32 kArrowHit = 0x20000; ///< DMG_ARROW: how a player hears the hit
    static constexpr u32 kBoltHit = 0x40000;  ///< DMG_FBALL

    s32 slot = kArrow;
    u32 flags = 0;
    f32 damage = 10.0f;
    f32 speed = 25.0f;
    f32 radius = 0.5f;           ///< of its body, for what it strikes
    f32 burstRadius = 0.0f;      ///< of the blast it ends in, none for nought
    Vec3 spin{0.0f, 0.0f, 0.0f}; ///< turns a second about each axis
    f32 weight = 30.0f;          ///< its fall a second a second: nought flies flat

    /** The medium kinds' shot and lob, as the original's table has them. */
    static EnemyMissileKind arrow();
    static EnemyMissileKind bomb();
    /** A kind's own shot (slot 2): the demons', ghosts', sorcerers', warlocks', worms' and
     * the garm's, each its own. */
    static EnemyMissileKind bolt(f32 damage, f32 speed, f32 radius, u32 flags = 0,
                                 f32 weight = 1.0f);
    /** The flags a player is hurt with: the kind's own and the slot's (arrow or bolt). */
    u32 hitFlags() const;
};

/** What a kind throws from a slot, as the original's table has it; nullopt for a kind and
 * slot with nothing. The slot a way throws from: the shooters (16, 23) the first, the
 * lobbers (17, 26) the second, the rest (28, 29 and the others) the third. */
std::optional<EnemyMissileKind> enemyMissileOf(s32 kind, s32 slot);
s32 missileSlotOfWay(s32 way);

/** Where a kind's missile leaves from (EnemyStartMissile's table): `height` over the body's
 * middle and `shift` along the throw, before the three units it starts out ahead. */
struct EnemyLaunchPoint {
    f32 height = 0.0f;
    f32 shift = 0.0f;
};
EnemyLaunchPoint enemyLaunchPointOf(s32 kind, s32 slot);

/** A throw as the thrower makes it. */
struct EnemyMissileLaunch {
    Vec3 body{0.0f};   ///< the thrower's middle, which the throw is aimed from
    Vec3 target{0.0f}; ///< the middle of what it is thrown at
    f32 facing = 0.0f; ///< the way the thrower faces
    EnemyLaunchPoint point;
    f32 speedScale = 1.0f; ///< the level's missile speed
    f32 aimError = 0.0f;   ///< the level's missile aim: the spread of a lob's error up or down
    const TreeModel* model = nullptr;
    s32 shooter = -1;
};

/** One of the swarm's missiles in flight. */
struct EnemyMissile {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    Vec3 turned{0.0f, 0.0f, 0.0f};
    EnemyMissileKind kind;
    const TreeModel* model = nullptr; ///< must outlive it
    s32 shooter = -1;
    f32 secondsLeft = 0.0f;
};

/** Where a missile ended, on a player or on the world (a lob bursts either way), or what
 * the burst it left reaches. */
struct EnemyMissileHit {
    s32 player = -1; ///< -1 for the world
    s32 shooter = -1;
    f32 damage = 0.0f;
    u32 flags = 0;
    f32 burstRadius = 0.0f; ///< the burst this starts, none for nought
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< the way its victim is pushed
    bool worldContact = false;        ///< expiry alone is not a surface hit
    bool fromBurst = false;           ///< a burst's reach rather than the missile itself
    f32 reach = 0.0f;                 ///< for a burst's hit on the swarm: how far it reaches
    std::string_view effect() const;
    std::string_view sound() const;
};

/**
 * The swarm's shots and lobs (EnemyStartMissile, ProcessEffects): a shot flies straight at
 * what it was aimed at, a lob rises and falls to land there, neither leading it; either
 * strikes the first player its body meets, or the world, and a lob bursts where it ends,
 * the burst growing as its harm fades.
 */
class EnemyMissiles {
public:
    static constexpr f32 kLife = 3.0f;     ///< StartMissile's lifetime
    static constexpr f32 kGravity = 40.0f; ///< the fall a straight lobVelocity assumes
    static constexpr f32 kLeastFlight = 0.3f;
    static constexpr f32 kLead = 3.0f;       ///< how far ahead of its launch point it starts
    static constexpr f32 kArrowDrop = -3.5f; ///< aimed this far under the target's middle
    static constexpr f32 kBombDrop = -5.5f;
    static constexpr f32 kAimSpread = 5.0f; ///< the error runs over this times the level's aim
    static constexpr f32 kFacing = 0.707f;  ///< nothing goes more than an eighth of a turn off
    static constexpr f32 kBombLightRadius = 5.0f;
    static constexpr f32 kBurstFade = 0.33f; ///< the share of a burst's life it no longer harms
    static constexpr f32 kBurstGrowth = 1.5f;
    static constexpr f32 kBurstKnockFrom = 5.0f; ///< under this a burst only hurts
    static constexpr f32 kBurstPush = 0.25f;

    explicit EnemyMissiles(u32 seed = 0x4D15u) : m_random(seed) {}

    /** Sends one off as `launch` throws it; false when its thrower faces too far off, or a
     * wall stands between it and the point it leaves from (and nothing goes). */
    bool launch(const EnemyMissileKind& kind, const EnemyMissileLaunch& launch,
                const WorldCollision* collision = nullptr);
    /** Sends one from `from` at `aim` (a body's middle) with no error, `speedScale` the
     * level's; for a shot along a line already chosen. */
    void launch(const EnemyMissileKind& kind, const Vec3& from, const Vec3& aim, f32 speedScale,
                const TreeModel* model, s32 shooter);
    /** The way a missile of `kind` leaves to reach `to` from `from` at `speed` along the
     * ground, a lob `error` high or low: a shot's straight, a lob's so that it falls there. */
    static Vec3 heading(const EnemyMissileKind& kind, const Vec3& from, const Vec3& to, f32 speed,
                        f32 error);
    /** A burst a lob left: growing to `radius` as its harm fades over `seconds`, sparing
     * `spared` (the player the lob itself struck). */
    void burst(const Vec3& position, f32 radius, f32 damage, u32 flags, f32 seconds, s32 spared);
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players);
    std::vector<EnemyMissileHit> takeHits();
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    /** The lobs' red lights and their bursts'. */
    void lights(std::vector<PointLight>& out) const;
    void clear();

    usize count() const { return m_missiles.size(); }
    usize burstCount() const { return m_bursts.size(); }
    const EnemyMissile& missile(usize index) const { return m_missiles[index]; }
    /** The velocity a lob leaves with to land `to` from `from` at `speed` along the ground. */
    static Vec3 lobVelocity(const Vec3& from, const Vec3& to, f32 speed);

private:
    struct Burst {
        Vec3 position{0.0f};
        f32 radius = 0.0f;
        f32 damage = 0.0f;
        u32 flags = 0;
        f32 seconds = 0.0f;
        f32 secondsLeft = 0.0f;
        std::vector<s32> spared; ///< players it has reached, or its lob did
        bool struckSwarm = false;
    };

    void stepBursts(f32 seconds, std::span<const EnemyView> players);

    std::vector<EnemyMissile> m_missiles;
    std::vector<Burst> m_bursts;
    std::vector<EnemyMissileHit> m_hits;
    std::mt19937 m_random;
};

} // namespace gdl::game
