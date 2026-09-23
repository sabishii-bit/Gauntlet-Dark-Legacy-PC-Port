#include "game/enemies/BossCoins.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

std::array<std::int32_t, BossCoins::kKinds> BossCoins::countsOf(std::int32_t realm) {
    if (realm < 0 || static_cast<std::size_t>(realm) >= kCounts.size()) {
        return {0, 0, 0};
    }
    return kCounts[static_cast<std::size_t>(realm)];
}

Vec3 BossCoins::yawed(const Vec3& v, float angle) {
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    return Vec3{v.x * c + v.z * s, v.y, v.z * c - v.x * s};
}

/** Each kind's coins are spread evenly over the arc, the first half a step in from one
 * edge and the last half a step in from the other; up to the original's room for them. */
std::vector<SpewedCoin> BossCoins::spray(std::int32_t realm, std::int32_t players,
                                         const Vec3& velocity, float halfAngle,
                                         std::mt19937& random) {
    std::vector<SpewedCoin> coins;
    std::uniform_real_distribution<float> spread(0.0f, kPaceSpread);
    const std::array<std::int32_t, kKinds> counts = countsOf(realm);
    for (std::size_t kind = 0; kind < kKinds; ++kind) {
        const std::int32_t n = std::max(players, 0) * counts[kind];
        if (n <= 0) {
            continue;
        }
        const float step = 2.0f * halfAngle / static_cast<float>(n);
        float angle = -halfAngle + 0.5f * step;
        for (std::int32_t i = 0; i < n && coins.size() < static_cast<std::size_t>(kMost); ++i) {
            const float pace = kPace[kind] + spread(random);
            coins.push_back(SpewedCoin{kNames[kind], kValues[kind], yawed(velocity * pace, angle)});
            angle += step;
        }
    }
    return coins;
}

} // namespace gdl::game
