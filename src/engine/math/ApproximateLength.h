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

/** The original's fast length of `v` in three dimensions (NormalVector, main.dol 0x800BDA98):
 * the fast horizontal length of x and z, then the fast horizontal length again with that as x
 * and y as z. */
f32 approximateLength(const Vec3& v);

/** `v` scaled by the reciprocal of its fast length, as the original's NormalVector
 * (0x800BDA98) scales it: the reciprocal is taken in double precision and rounded once to
 * single, then multiplies each component in single precision. A vector of no length is left
 * as it is. The fast length is approximate, so the result is only near unit length. */
Vec3 approximateUnit(const Vec3& v);

/** `v` laid flat and scaled by the reciprocal of its fast horizontal length, as the original's
 * NormalVector2D (0x800BD938) scales it: the reciprocal is rounded as approximateUnit rounds
 * it, and y becomes zero. A vector of no horizontal length keeps its x and z. The result is
 * only near unit length. */
Vec3 approximateFlatUnit(const Vec3& v);

} // namespace gdl
