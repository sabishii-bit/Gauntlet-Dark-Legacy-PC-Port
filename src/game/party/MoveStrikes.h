#pragma once

#include <memory>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/characters/ClassData.h"
#include "game/combat/StrikeHit.h"

namespace gdl::game {

/**
 * The strikes of the party's moves, as the original's class data describes them: a burst
 * expands after its delay over its effect's lifetime; what flies goes off along the
 * character's facing at its speed, harming what it passes, until its time is up or a wall
 * stops it.
 */
class MoveStrikes {
public:
    MoveStrikes() = default;
    MoveStrikes(const MoveStrikes&) = delete;
    MoveStrikes& operator=(const MoveStrikes&) = delete;
    MoveStrikes(MoveStrikes&&) = delete;
    MoveStrikes& operator=(MoveStrikes&&) = delete;
    ~MoveStrikes() = default;
    /** One strike under way. */
    struct Strike {
        u32 id = 0;
        s32 owner = 0;
        bool flies = false;
        bool expanding = false;
        bool collidesWorld = true;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 facing{0.0f, 0.0f, 1.0f};
        f32 speed = 0.0f;
        f32 radius = 0.0f;
        f32 arc = -1.0f;
        f32 damage = 0.0f;
        f32 delayLeft = 0.0f;
        f32 launchIn = 0.0f; ///< DAMG 0x800 holds motion until the birth effect morphs
        f32 secondsLeft = 0.0f;
        f32 damageTime = 0.0f; ///< effect lifetime less the burst's wind-up
        bool operator==(const Strike&) const = default;
        template <class D> friend void visitNativeState(D& digest, const Strike& value) {
            digest.template fields<15>(value);
        }
    };

private:
    struct Simulation {
        std::vector<Strike> strikes;
        u32 next = 1;
        template <class D> friend void visitNativeState(D& digest, const Simulation& value) {
            digest.template fields<2>(value);
        }
    };

public:
    class LocalSnapshot {
        friend class MoveStrikes;
        Simulation m_state;
        const MoveStrikes* m_owner = nullptr;
        std::weak_ptr<const u8> m_lifetime;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

    /** What a character's own harm is multiplied by when a strike's amount is negative. */
    static f32 damageOf(const MoveStrike& strike, f32 ownDamage);
    /** Where a strike starts: its offset from `position`, turned to `facing`. */
    static Vec3 originOf(const MoveStrike& strike, const Vec3& position, const Vec3& facing);

    /** Starts `strike` for `owner` standing at `position` and facing `facing` (level, unit
     * length); its number, which its hits carry. Area lifetime comes from the first effect's
     * sequence, not the projectile morph time in the damage row. */
    u32 start(const MoveStrike& strike, s32 owner, const Vec3& position, const Vec3& facing,
              f32 ownDamage, f32 effectSeconds = 0.0f);
    std::vector<StrikeHit> update(f32 seconds, const WorldCollision* collision);
    /** Reflect a point strike at armor, keeping its owner, or end it on body contact. */
    void hitPlayer(u32 id, bool reflected, const Vec3& from);
    /** Carry an area on its posed parent. Detached flying strikes are never reparented. */
    void placeArea(u32 id, const Mat4& placement);
    void stop(u32 id);
    void clear();

    usize count() const { return m_sim.strikes.size(); }
    const Strike& strike(usize index) const { return m_sim.strikes[index]; }
    /** The strike numbered `id`, or null once it is over. */
    const Strike* find(u32 id) const;

private:
    Simulation m_sim;
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_localEpoch = 0;
};

} // namespace gdl::game
