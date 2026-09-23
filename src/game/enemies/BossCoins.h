#pragma once

#include <array>
#include <random>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** One coin a boss throws out: which, worth how much, and how it leaves. */
struct SpewedCoin {
    std::string_view name; ///< the level's item record: `COIN_BRONZE`, `COIN_SILVER`, `COIN_GOLD`
    s32 value = 0;
    Vec3 velocity{0.0f, 0.0f, 0.0f};
};

/**
 * The coins a boss spews as it dies, as the original counts them: for each player in the
 * game, the realm's own number of bronze (500), silver (1000) and gold (5000) coins, each
 * kind fanned evenly across the death's arc about the way it throws, a little slower than
 * the throw and a little unevenly (bronze at 0.85 to 0.95 of it, silver 0.8 to 0.9, gold
 * 0.75 to 0.85). The coins are thrown as the level's items (`PlacedItems::throwItem`), and
 * cannot be taken for their first two seconds. Temple Skorne throws four relics instead
 * (SkorneRelics); the tower and the battlefield throw nothing.
 */
class BossCoins {
public:
    static constexpr usize kKinds = 3;
    static constexpr std::array<std::string_view, kKinds> kNames{"COIN_BRONZE", "COIN_SILVER",
                                                                 "COIN_GOLD"};
    static constexpr std::array<s32, kKinds> kValues{500, 1000, 5000};
    static constexpr std::array<f32, kKinds> kPace{0.85f, 0.8f, 0.75f}; ///< of the throw
    static constexpr f32 kPaceSpread = 0.1f;
    /** Each realm's coins of each kind, by realm number (the castle 1 to the tower 13). */
    static constexpr std::array<std::array<s32, kKinds>, 14> kCounts{{{0, 0, 0},
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
    static constexpr s32 kMost = 32;
    static constexpr f32 kNoGrabSeconds = 2.0f;

    /** The coins of `realm` for `players`, thrown at `velocity` and fanned `halfAngle`
     * radians each side of it. */
    static std::vector<SpewedCoin> spray(s32 realm, s32 players, const Vec3& velocity,
                                         f32 halfAngle, std::mt19937& random);
    /** How many coins of each kind `realm` throws for one player. */
    static std::array<s32, kKinds> countsOf(s32 realm);
    /** `v` turned `angle` radians about the upright. */
    static Vec3 yawed(const Vec3& v, f32 angle);
};

} // namespace gdl::game
