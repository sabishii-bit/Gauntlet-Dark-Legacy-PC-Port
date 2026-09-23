#pragma once

#include <array>
#include <cstddef>
#include <random>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::game {

/** One coin a boss throws out: which, worth how much, and how it leaves. */
struct SpewedCoin {
    std::string_view name; ///< the level's item record: `COIN_BRONZE`, `COIN_SILVER`, `COIN_GOLD`
    int value = 0;
    Vec3 velocity{0.0f, 0.0f, 0.0f};
};

/**
 * The coins a boss spews as it dies, as the original counts them: for each player in the
 * game, the realm's own number of bronze (500), silver (1000) and gold (5000) coins, each
 * kind fanned evenly across the death's arc about the way it throws, a little slower than
 * the throw and a little unevenly (bronze at 0.85 to 0.95 of it, silver 0.8 to 0.9, gold
 * 0.75 to 0.85). The coins are thrown as the level's items (`PlacedItems::throwItem`), and
 * cannot be taken for their first two seconds. The underworld's demon throws its four
 * relics instead, which is not done yet; the tower and the battlefield throw nothing.
 */
class BossCoins {
public:
    static constexpr std::size_t kKinds = 3;
    static constexpr std::array<std::string_view, kKinds> kNames{"COIN_BRONZE", "COIN_SILVER",
                                                                 "COIN_GOLD"};
    static constexpr std::array<int, kKinds> kValues{500, 1000, 5000};
    static constexpr std::array<float, kKinds> kPace{0.85f, 0.8f, 0.75f}; ///< of the throw
    static constexpr float kPaceSpread = 0.1f;
    /** Each realm's coins of each kind, by realm number (the castle 1 to the tower 13). */
    static constexpr std::array<std::array<int, kKinds>, 14> kCounts{{{0, 0, 0},
                                                                      {2, 1, 1},
                                                                      {0, 5, 0},
                                                                      {2, 1, 2},
                                                                      {2, 0, 2},
                                                                      {0, 0, 0},
                                                                      {0, 0, 4},
                                                                      {4, 1, 0},
                                                                      {0, 0, 0},
                                                                      {0, 3, 2},
                                                                      {0, 0, 3},
                                                                      {0, 4, 1},
                                                                      {0, 0, 0},
                                                                      {0, 0, 0}}};
    static constexpr int kMost = 32;
    static constexpr float kNoGrabSeconds = 2.0f;

    /** The coins of `realm` for `players`, thrown at `velocity` and fanned `halfAngle`
     * radians each side of it. */
    static std::vector<SpewedCoin> spray(int realm, int players, const Vec3& velocity,
                                         float halfAngle, std::mt19937& random);
    /** How many coins of each kind `realm` throws for one player. */
    static std::array<int, kKinds> countsOf(int realm);
    /** `v` turned `angle` radians about the upright. */
    static Vec3 yawed(const Vec3& v, float angle);
};

} // namespace gdl::game
