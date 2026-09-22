#include "game/enemies/BossCoins.h"

#include <cmath>

namespace gdl::game {

std::array<s32, BossCoins::kKinds> BossCoins::countsOf(s32 realm) {
    if (realm < 0 || static_cast<usize>(realm) >= kCounts.size()) {
        return {0, 0, 0};
    }
    return kCounts[static_cast<usize>(realm)];
}

Vec3 BossCoins::yawed(const Vec3& v, f32 angle) {
    const f32 s = std::sin(angle);
    const f32 c = std::cos(angle);
    return Vec3{v.x * c + v.z * s, v.y, v.z * c - v.x * s};
}

/** Each kind's coins are spread evenly over the arc, the first half a step in from one
 * edge and the last half a step in from the other; up to the original's room for them. */
std::vector<SpewedCoin> BossCoins::spray(s32 realm, s32 players, const Vec3& velocity,
                                        f32 halfAngle, std::mt19937& random) {
    std::vector<SpewedCoin> coins;
    std::uniform_real_distribution<f32> spread(0.0f, kPaceSpread);
    const std::array<s32, kKinds> counts = countsOf(realm);
    for (usize kind = 0; kind < kKinds; ++kind) {
        const s32 n = std::max(players, 0) * counts[kind];
        if (n <= 0) {
            continue;
        }
        const f32 step = 2.0f * halfAngle / static_cast<f32>(n);
        f32 angle = -halfAngle + 0.5f * step;
        for (s32 i = 0; i < n && coins.size() < static_cast<usize>(kMost); ++i) {
            const f32 pace = kPace[kind] + spread(random);
            coins.push_back(SpewedCoin{kNames[kind], kValues[kind], yawed(velocity * pace, angle)});
            angle += step;
        }
    }
    return coins;
}

} // namespace gdl::game
