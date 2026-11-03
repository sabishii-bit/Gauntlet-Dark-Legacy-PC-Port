#pragma once
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** What stands between a combatant's blow and a player: asked for bosses' and great ones'
 * breath and wide areas alike. */
class CombatCover {
public:
    static constexpr f32 kBreathProbeRadius = 0.5f; ///< how wide breath is taken to be
    static constexpr f32 kAreaProbeRadius = 0.1f;   ///< how wide a wide area's blow is taken to be

    virtual ~CombatCover() = default;
    /** Whether something shelters `to` from breath let out at `from`. */
    virtual bool blocksBreath(const Vec3& from, const Vec3& to) const = 0;
    /** Whether something shelters `to` from a wide area's blow centred on `from`. */
    virtual bool blocksArea(const Vec3& from, const Vec3& to) const = 0;
};
} // namespace gdl::game
