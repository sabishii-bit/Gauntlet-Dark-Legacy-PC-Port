#pragma once

#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/players/ClassData.h"

namespace gdl::game {

/** Harm a strike does this update: to whatever is within `radius` of `centre` and, when the
 * arc is not all round, no further from `facing` than it allows. */
struct StrikeHit {
    u32 strike = 0; ///< which strike it is, the same for every hit of one that flies
    s32 owner = 0;
    Vec3 centre{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 arc = -1.0f;
    Vec3 facing{0.0f, 0.0f, 1.0f};
    f32 damage = 0.0f;

    /** Whether something standing at `position` is caught by it. */
    bool reaches(const Vec3& position, f32 targetRadius, f32 targetHeight) const;
};

/**
 * The strikes of the party's moves, as the original's class data describes them: a burst
 * harms what is about it once, its delay after it starts; what flies goes off along the
 * character's facing at its speed, harming what it passes, until its time is up or a wall
 * stops it.
 */
class MoveStrikes {
public:
    /** One strike under way. */
    struct Strike {
        u32 id = 0;
        s32 owner = 0;
        bool flies = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 facing{0.0f, 0.0f, 1.0f};
        f32 speed = 0.0f;
        f32 radius = 0.0f;
        f32 arc = -1.0f;
        f32 damage = 0.0f;
        f32 delayLeft = 0.0f;
        f32 secondsLeft = 0.0f; ///< of what flies
    };

    /** What a character's own harm is multiplied by when a strike's amount is negative. */
    static f32 damageOf(const MoveStrike& strike, f32 ownDamage);
    /** Where a strike starts: its offset from `position`, turned to `facing`. */
    static Vec3 originOf(const MoveStrike& strike, const Vec3& position, const Vec3& facing);

    /** Starts `strike` for `owner` standing at `position` and facing `facing` (level, unit
     * length); its number, which its hits carry. */
    u32 start(const MoveStrike& strike, s32 owner, const Vec3& position, const Vec3& facing,
              f32 ownDamage);
    std::vector<StrikeHit> update(f32 seconds, const WorldCollision* collision);
    void clear();

    usize count() const { return m_strikes.size(); }
    const Strike& strike(usize index) const { return m_strikes[index]; }
    /** The strike numbered `id`, or null once it is over. */
    const Strike* find(u32 id) const;

private:
    std::vector<Strike> m_strikes;
    u32 m_next = 1;
};

} // namespace gdl::game
