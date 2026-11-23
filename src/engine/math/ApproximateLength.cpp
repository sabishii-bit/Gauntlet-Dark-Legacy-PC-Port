#include "engine/math/ApproximateLength.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gdl {
namespace {
/** A component below this leaves the other as the length (0x80348DA0). */
constexpr f64 kNegligibleComponent = 0.0001;
/** The bands split the shorter component's share of the longer into eighths. */
constexpr f64 kBandWidth = 0.125;
/** The fraction of the shorter component each band adds (0x80348E08..0x80348E60). */
constexpr std::array<f64, 8> kBandSlopes{0.064, 0.124, 0.181, 0.236, 0.287, 0.333, 0.376, 0.414};
/** What a vector of no length is scaled by, which leaves it as it is (0x80348E90). */
constexpr f32 kNoLengthScale = 1.0f;

/** What a vector of fast length `length` is scaled by to near unit length. */
f32 reciprocalScale(f32 length) {
    // The original divides the double 1.0 (0x80348E78) and rounds the quotient once.
    return length <= 0.0f ? kNoLengthScale : static_cast<f32>(1.0 / static_cast<f64>(length));
}
} // namespace

f32 approximateFlatLength(const Vec3& v) {
    const f64 x = std::abs(static_cast<f64>(v.x));
    const f64 z = std::abs(static_cast<f64>(v.z));
    if (x < kNegligibleComponent) {
        return static_cast<f32>(z);
    }
    if (z < kNegligibleComponent) {
        return static_cast<f32>(x);
    }
    const f64 high = std::max(x, z);
    const f64 low = std::min(x, z);
    // A share exactly on a band's upper edge stays in that band, as the original's strict
    // comparisons leave it.
    for (usize band = 0; band + 1 < kBandSlopes.size(); ++band) {
        if (low <= high * kBandWidth * static_cast<f64>(band + 1)) {
            return static_cast<f32>(high + kBandSlopes[band] * low);
        }
    }
    return static_cast<f32>(high + kBandSlopes.back() * low);
}

f32 approximateLength(const Vec3& v) {
    return approximateFlatLength(Vec3{approximateFlatLength(v), 0.0f, v.y});
}

Vec3 approximateUnit(const Vec3& v) {
    return v * reciprocalScale(approximateLength(v));
}

Vec3 approximateFlatUnit(const Vec3& v) {
    const f32 scale = reciprocalScale(approximateFlatLength(v));
    return Vec3{v.x * scale, 0.0f, v.z * scale};
}

} // namespace gdl
