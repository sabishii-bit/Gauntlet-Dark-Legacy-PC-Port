#pragma once
#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/EnemyMind.h"

namespace gdl::game {
/** A great one's round of the level's lookouts (CritterNewInst, CritterGetTarget): from the
 * chained lookout nearest where it was placed, within ten, to each one the last names, taken
 * as reached within a unit; over when the chain ends, when it targets a player, or when it is
 * hurt. The route is borrowed and must outlive the round. */
class CritterPatrol {
public:
    static constexpr f32 kStartReach = 10.0f; ///< FindClosestWaypoint's reach at birth
    static constexpr f32 kReached = 1.0f;     ///< within this of a lookout it goes on to the next

    /** Starts the round from the chained lookout nearest `position`; none within reach, and
     * there is no round. `sight` over nought bounds the target score of the players taken
     * meanwhile (the placement's radius at the level's sight scale, visrad). */
    void start(const LookoutRoute* route, const Vec3& position, f32 sight = 0.0f);
    void end();
    bool active() const { return m_lookout >= 0; }
    f32 sight() const { return m_sight; }
    /** The lookout it makes for, by its place in the route; -1 for none. */
    s32 lookout() const { return m_lookout; }
    /** Where it makes for from `position`, going on past any lookout reached; none once the
     * round is over. */
    std::optional<Vec3> aim(const Vec3& position);

private:
    /** Whether a lookout is part of a chain: it names another, not itself. */
    static bool chained(const LookoutRoute& route, usize index);
    const LookoutRoute* m_route = nullptr;
    s32 m_lookout = -1;
    f32 m_sight = 0.0f;
};
} // namespace gdl::game
