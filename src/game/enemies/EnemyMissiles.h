#pragma once

#include <optional>
#include <span>
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
    static constexpr u32 kKnockBack = 0x10;

    u32 flags = 0;
    f32 damage = 10.0f;
    f32 speed = 25.0f;
    f32 radius = 0.5f;           ///< of its body, for what it strikes
    f32 burstRadius = 0.0f;      ///< of the blast it ends in, none for nought
    Vec3 spin{0.0f, 0.0f, 0.0f}; ///< turns a second about each axis
    f32 weight = 30.0f;          ///< how it falls: nought flies flat

    /** The medium kinds' shot and lob, as the original's table has them. */
    static EnemyMissileKind arrow();
    static EnemyMissileKind bomb();
    /** A kind's own shot (slot 2): the demons', ghosts', sorcerers', warlocks', worms' and
     * the garm's, each its own. */
    static EnemyMissileKind bolt(f32 damage, f32 speed, f32 radius, u32 flags = 0);
};

/** What a kind throws from a slot, as the original's table has it; nullopt for a kind and
 * slot with nothing. The slot a way throws from: the shooters (16, 23) the first, the
 * lobbers (17, 26) the second, the rest (28, 29 and the others) the third. */
std::optional<EnemyMissileKind> enemyMissileOf(s32 kind, s32 slot);
s32 missileSlotOfWay(s32 way);

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

/** Where a missile ended: on a player, or on the world; a lob bursts either way. */
struct EnemyMissileHit {
    s32 player = -1; ///< -1 for the world
    s32 shooter = -1;
    f32 damage = 0.0f;
    u32 flags = 0;
    f32 burstRadius = 0.0f;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f};
};

/**
 * The swarm's shots and lobs: a shot flies straight at what it was aimed at, at the level's
 * missile speed, a lob rises and falls to land there; either strikes the first player its
 * body meets, or the world, and a lob bursts where it ends.
 */
class EnemyMissiles {
public:
    static constexpr f32 kLife = 6.0f;
    static constexpr f32 kGravity = 40.0f; ///< a lob's fall a second a second
    static constexpr f32 kLeastFlight = 0.3f;

    /** Sends one from `from` at `aim` (a body's middle), `speedScale` the level's. */
    void launch(const EnemyMissileKind& kind, const Vec3& from, const Vec3& aim, f32 speedScale,
                const TreeModel* model, s32 shooter);
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players);
    std::vector<EnemyMissileHit> takeHits();
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();

    usize count() const { return m_missiles.size(); }
    const EnemyMissile& missile(usize index) const { return m_missiles[index]; }
    /** The velocity a lob leaves with to land `to` from `from` at `speed` along the ground. */
    static Vec3 lobVelocity(const Vec3& from, const Vec3& to, f32 speed);

private:
    std::vector<EnemyMissile> m_missiles;
    std::vector<EnemyMissileHit> m_hits;
};

} // namespace gdl::game
