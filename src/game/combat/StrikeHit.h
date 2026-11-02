#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

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
    Vec3 from{0.0f}; ///< beginning of a flying strike's swept contact segment
    bool swept = false;
    f32 hitGap = 0.0f; ///< area-contact immunity, independent of projectile contacts
    bool operator==(const StrikeHit&) const = default;

    /** Whether something standing at `position` is caught by it. */
    bool reaches(const Vec3& position, f32 targetRadius, f32 targetHeight) const;
};

} // namespace gdl::game
