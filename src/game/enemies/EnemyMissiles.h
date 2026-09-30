#pragma once

#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/Enemies.h"
#include "game/world/PlayerMissiles.h"

namespace gdl::game {

/** What the original's table says of a kind's missile: its harm, pace, size and flight. */
struct EnemyMissileKind {
    static constexpr s32 kArrow = 0;     ///< the slot a shot takes
    static constexpr s32 kBomb = 1;      ///< and a lob
    static constexpr s32 kBolt = 2;      ///< and a kind's own shot
    static constexpr s32 kDeathShot = 3; ///< the garm brood's burst, with no sound or spark
    static constexpr u32 kKnockBack = 0x10;
    static constexpr u32 kKnockDown = 0x20;
    static constexpr u32 kArrowHit = 0x20000; ///< DMG_ARROW: how a player hears the hit
    static constexpr u32 kBoltHit = 0x40000;  ///< DMG_FBALL
    static constexpr u32 kPierces = 0x100000; ///< DMG_SUPER: through players and items

    s32 slot = kArrow;
    u32 flags = 0;
    f32 damage = 10.0f;
    f32 speed = 25.0f;
    f32 radius = 0.5f;           ///< of its body, for what it strikes
    f32 burstRadius = 0.0f;      ///< of the blast it ends in, none for nought
    Vec3 spin{0.0f, 0.0f, 0.0f}; ///< turns a second about each axis
    f32 weight = 30.0f;          ///< its fall a second a second: nought flies flat
    /** Seconds it stays where it starts before it flies, striking what stands in it meanwhile
     * (the brood's death burst, held for its sequence before its morph moves). */
    f32 held = 0.0f;
    /** Whether it goes through walls, floors and the level's items alike (an effect without
     * the world-collision flag). */
    bool throughWorld = false;

    /** The medium kinds' shot and lob, as the original's table has them. */
    static EnemyMissileKind arrow();
    static EnemyMissileKind bomb();
    /** A kind's own shot (slot 2): the demons', ghosts', sorcerers', warlocks', worms' and
     * the garm's, each its own. */
    static EnemyMissileKind bolt(f32 damage, f32 speed, f32 radius, u32 flags = 0,
                                 f32 weight = 1.0f);
    /** The garm brood's death shot (StartEnemyDeathFX): twenty a second and weightless for
     * three seconds, three units wide, fifty of harm with knock-down, through the players it
     * hurts and through the world; `held` is the launcher's, the burst's length. */
    static EnemyMissileKind deathShot();
    /** The flags a player is hurt with: the kind's own and the slot's (arrow or bolt). */
    u32 hitFlags() const;
    /** Whether it goes through the players it hurts and the items in its way (the garm's). */
    bool pierces() const { return (flags & kPierces) != 0; }
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
    f32 lived = 0.0f;
    f32 heldLeft = 0.0f;    ///< seconds it still stays where it started
    f32 scale = 1.0f;       ///< the enemy shrinkers' scale as it was thrown
    bool reflected = false; ///< sent back by a player's armour: it now strikes the swarm
    /** Players a piercing missile has hurt, and how long it leaves each alone. */
    std::vector<std::pair<s32, f32>> pierced;
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
    bool fromBurst = false;           ///< a blast's reach rather than the missile itself
    bool ricochet = false;            ///< only the sound of a missile turned by armour
    s32 target = -1;      ///< one of the swarm (by the id it was offered under) a blast reached
    s32 worldObject = -1; ///< actual collision owner, never an item or creature id
    bool liquid = false;  ///< replaces impact audio with the water cue
    std::string_view effect() const;
    std::string_view sound() const;
};

/** An area one of the swarm's blasts harms as it grows (ProcessEffects' mode 1): in each of
 * its stages it spreads from a third of its radius to the whole over the first two thirds of
 * the stage, its harm falling from one and a half times to nothing as it does, and is then
 * harmless until the next. */
struct EnemyBlast {
    static constexpr u32 kGas = 0x800;

    Vec3 position{0.0f};
    f32 radius = 3.0f;
    f32 damage = 0.0f;
    u32 flags = 0;
    std::vector<f32> stages; ///< how long each lasts
    s32 spared = -1;         ///< a player it never reaches: the one its lob struck
    bool lit = false;        ///< the lob's red light rides on it
};

/** Something of the level's in the way of the swarm's missiles besides the world
 * (fn_8005ED44, SfxSkipItem): doors, chests, barrels, generators, bottles lying about, the
 * triggers that are shot and raised tent walls stop one unharmed; a standing safe rock takes
 * its blow, and a piercing bolt stops only at a rock that blow leaves standing. */
struct MissileStop {
    Obstacle box;
    s32 rock = -1;      ///< the safe rock it is, or -1
    s32 rockHealth = 0; ///< that rock's health and armour
    s32 rockArmor = 0;

    static MissileStop of(const Obstacle& box) { return MissileStop{.box = box}; }
};

/** Where a gas blast's ring stands this step, for the level's food. */
struct GasReach {
    Vec3 position{0.0f};
    f32 radius = 0.0f;
    f32 damage = 0.0f;
};

/** A missile's blow on a safe rock, for the level to deal. */
struct RockHit {
    usize rock = 0;
    f32 damage = 0.0f;
};

/**
 * The swarm's shots and lobs (EnemyStartMissile, ProcessEffects): a shot flies straight at
 * what it was aimed at, a lob rises and falls to land there, neither leading it; either
 * strikes the first player its body meets, or the world, and a lob bursts where it ends,
 * the burst growing as its harm fades.
 */
class EnemyMissiles {
public:
    static constexpr f32 kLife = 3.0f;     ///< StartMissile's lifetime, and a death shot's flight
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
    static constexpr f32 kGasGap = 0.5f;             ///< a gas blast hurts a player this often
    static constexpr f32 kBlastSlack = 1.0f / 15.0f; ///< past a stage a hit is held off
    static constexpr f32 kSwarmGap = 1.0f;       ///< at least this before one of the swarm again
    static constexpr f32 kSightFrom = 10.0f;     ///< past this a wall between shelters a player
    static constexpr f32 kReflectedMost = 15.0f; ///< what a missile armour sent back can harm
    static constexpr f32 kReflectedLife = 10.0f; ///< and the longest it then has left
    static constexpr f32 kRicochetGap = 1.0f;    ///< one ricochet heard a second
    static constexpr f32 kPierceGap = 0.25f;     ///< a player pierced is hurt again after this
    static constexpr f32 kGrowth = 1.0f / 3.0f;  ///< a piercing missile grows to size over this
    static constexpr f32 kSmallest = 0.01f;      ///< from this

    explicit EnemyMissiles(u32 seed = 0x4D15u) : m_random(seed) {}

    /** The scale the party's enemy shrinkers hold the swarm at (`EnemyShrink`): what is thrown
     * meanwhile is that size and does half (EnemyStartMissile). */
    void setShrink(f32 scale) { m_shrink = scale; }
    f32 shrink() const { return m_shrink; }

    /** Sends one off as `launch` throws it; false when its thrower faces too far off, or a
     * wall or one of the `items` stands between it and the point it leaves from (and nothing
     * goes). */
    bool launch(const EnemyMissileKind& kind, const EnemyMissileLaunch& launch,
                const WorldCollision* collision = nullptr, std::span<const Obstacle> items = {});
    /** Sends one from `from` at `aim` (a body's middle) with no error, `speedScale` the
     * level's; for a shot along a line already chosen. */
    void launch(const EnemyMissileKind& kind, const Vec3& from, const Vec3& aim, f32 speedScale,
                const TreeModel* model, s32 shooter);
    /** The way a missile of `kind` leaves to reach `to` from `from` at `speed` along the
     * ground, a lob `error` high or low: a shot's straight, a lob's so that it falls there. */
    static Vec3 heading(const EnemyMissileKind& kind, const Vec3& from, const Vec3& to, f32 speed,
                        f32 error);
    /** A burst a lob left: growing to `radius` as its harm fades over `seconds`, sparing
     * `spared` (the player the lob itself struck), with the lob's light on it. */
    void burst(const Vec3& position, f32 radius, f32 damage, u32 flags, f32 seconds, s32 spared);
    /** Sets a blast going. */
    void blast(EnemyBlast blast);
    /** Moves everything in flight and grows the blasts over `seconds`. Missiles stop at the
     * world and at the level's `items` (`MissileStop`), a piercing one going through the
     * players it hurts; armour that reflects sends one back, after which it strikes the
     * `swarm`, as the blasts do (each by the id it is offered under). */
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players,
                std::span<const MissileTarget> swarm = {}, std::span<const MissileStop> items = {});
    std::vector<EnemyMissileHit> takeHits();
    /** The blows on safe rocks since the last call. */
    std::vector<RockHit> takeRockHits();
    /** Where the gas blasts reached since the last call. */
    std::vector<GasReach> takeGasReaches() { return std::exchange(m_gasReaches, {}); }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    /** The lobs' red lights and their bursts'. */
    void lights(std::vector<PointLight>& out) const;
    void clear();

    usize count() const { return m_missiles.size(); }
    usize burstCount() const { return m_bursts.size(); }
    const EnemyMissile& missile(usize index) const { return m_missiles[index]; }
    /** Whether the world stands between `from` and `to` for a body of `radius`. */
    static bool walled(const WorldCollision& collision, const Vec3& from, const Vec3& to,
                       f32 radius);
    /** The velocity a lob leaves with to land `to` from `from` at `speed` along the ground. */
    static Vec3 lobVelocity(const Vec3& from, const Vec3& to, f32 speed);

private:
    /** `kind` as the shrinkers leave it: its harm halved while the swarm is shrunk. */
    EnemyMissileKind thrown(const EnemyMissileKind& kind) const;
    /** Who a blast has reached, and for how long it leaves them alone. */
    struct Held {
        s32 id = -1;
        f32 secondsLeft = 0.0f;
    };
    struct Burst {
        EnemyBlast blast;
        usize stage = 0;
        f32 stageLeft = 0.0f;
        std::vector<Held> players;
        std::vector<Held> swarm;
    };

    bool stopped(const EnemyMissile& missile, std::span<const MissileStop> items, const Vec3& from,
                 const Vec3& to, f32 radius);
    void stepBursts(f32 seconds, const WorldCollision* collision,
                    std::span<const EnemyView> players, std::span<const MissileTarget> swarm);

    std::vector<EnemyMissile> m_missiles;
    std::vector<Burst> m_bursts;
    std::vector<EnemyMissileHit> m_hits;
    std::vector<RockHit> m_rockHits;
    std::vector<GasReach> m_gasReaches;
    std::mt19937 m_random;
    f32 m_shrink = 1.0f;
    f32 m_ricochetIn = 0.0f; ///< before another ricochet is heard
};

} // namespace gdl::game
