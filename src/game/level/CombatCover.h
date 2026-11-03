#pragma once
#include "engine/math/Math.h"

namespace gdl::game {
/** What stands between a combatant's blow and a player: asked for bosses' and great ones'
 * breath and wide areas alike. */
class CombatCover {
public:
    virtual ~CombatCover() = default;
    /** Whether something shelters `to` from breath let out at `from`. */
    virtual bool blocksBreath(const Vec3& from, const Vec3& to) const = 0;
    /** Whether something shelters `to` from a wide area's blow centred on `from`. */
    virtual bool blocksArea(const Vec3& from, const Vec3& to) const = 0;
};
} // namespace gdl::game
