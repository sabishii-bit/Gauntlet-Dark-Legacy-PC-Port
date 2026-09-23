#pragma once

#include <cstddef>
#include <vector>

#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/players/ClassData.h"

namespace gdl::game {

/** Harm a strike does this update: to whatever is within `radius` of `centre` and, when the
 * arc is not all round, no further from `facing` than it allows. */
struct StrikeHit {
    unsigned int strike = 0; ///< which strike it is, the same for every hit of one that flies
    int owner = 0;
    Vec3 centre{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float arc = -1.0f;
    Vec3 facing{0.0f, 0.0f, 1.0f};
    float damage = 0.0f;

    /** Whether something standing at `position` is caught by it. */
    bool reaches(const Vec3& position, float targetRadius, float targetHeight) const;
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
        unsigned int id = 0;
        int owner = 0;
        bool flies = false;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 facing{0.0f, 0.0f, 1.0f};
        float speed = 0.0f;
        float radius = 0.0f;
        float arc = -1.0f;
        float damage = 0.0f;
        float delayLeft = 0.0f;
        float secondsLeft = 0.0f; ///< of what flies
    };

    /** What a character's own harm is multiplied by when a strike's amount is negative. */
    static float damageOf(const MoveStrike& strike, float ownDamage);
    /** Where a strike starts: its offset from `position`, turned to `facing`. */
    static Vec3 originOf(const MoveStrike& strike, const Vec3& position, const Vec3& facing);

    /** Starts `strike` for `owner` standing at `position` and facing `facing` (level, unit
     * length); its number, which its hits carry. */
    unsigned int start(const MoveStrike& strike, int owner, const Vec3& position,
                       const Vec3& facing, float ownDamage);
    std::vector<StrikeHit> update(float seconds, const WorldCollision* collision);
    void clear();

    std::size_t count() const { return m_strikes.size(); }
    const Strike& strike(std::size_t index) const { return m_strikes[index]; }
    /** The strike numbered `id`, or null once it is over. */
    const Strike* find(unsigned int id) const;

private:
    std::vector<Strike> m_strikes;
    unsigned int m_next = 1;
};

} // namespace gdl::game
