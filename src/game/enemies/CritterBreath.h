#pragma once

#include "engine/core/Types.h"

#include "game/enemies/CritterData.h"

namespace gdl::game {
/** The damaging segment emitted by an animated body part. Its offsets use the
 * full node transform, but its reach is independent of the node's scale. */
struct CritterBreath {
    Vec3 origin{0.0f};
    Vec3 end{0.0f};

    static CritterBreath fromNode(const Mat4& node, const CritterDamage& damage);

    /** Tests the closest point on the segment against the expanded upright
     * cylinder. The horizontal distance window is measured before expansion. */
    bool touches(const CritterDamage& damage, const Vec3& centre, f32 radius, f32 halfHeight) const;
};
} // namespace gdl::game
