#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** The original's fast horizontal length of `v` (fqdist, main.dol 0x800BCB44): the longer of
 * |x| and |z| plus one of eight banded fractions of the shorter, rounded once to single
 * precision; y is ignored. A component under 0.0001 leaves the other as the length, |x| tested
 * first. Code that ranks or compares distances as the original does uses this, since the
 * Euclidean length can order near ties differently. */
f32 approximateFlatLength(const Vec3& v);

} // namespace gdl
