#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/enemies/BossCoins.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr float kPi = std::numbers::pi_v<float>;

TEST_CASE("a boss throws its realm's coins for each player, fanned over the death's arc",
          "[game][enemies]") {
    // The town's lich throws four bronze and one silver a player; the castle's chimera two
    // bronze, one silver and one gold; the tower's and the battlefield's bosses nothing.
    REQUIRE(BossCoins::countsOf(7) == std::array<int, 3>{4, 1, 0});
    REQUIRE(BossCoins::countsOf(1) == std::array<int, 3>{2, 1, 1});
    REQUIRE(BossCoins::countsOf(13) == std::array<int, 3>{0, 0, 0});
    REQUIRE(BossCoins::countsOf(-1) == std::array<int, 3>{0, 0, 0});
    REQUIRE(BossCoins::countsOf(99) == std::array<int, 3>{0, 0, 0});
    std::mt19937 random{7};
    const Vec3 throwUp{0.0f, 20.0f, 30.0f};
    // Two players in the town: eight bronze all round and two silver.
    const std::vector<SpewedCoin> coins = BossCoins::spray(7, 2, throwUp, kPi, random);
    REQUIRE(coins.size() == 10);
    for (std::size_t i = 0; i < 8; ++i) {
        REQUIRE(coins[i].name == "COIN_BRONZE");
        REQUIRE(coins[i].value == 500);
        // Slower than the throw and never faster than the bronze's share of it.
        const float pace = glm::length(coins[i].velocity) / glm::length(throwUp);
        REQUIRE(pace >= 0.85f);
        REQUIRE(pace <= 0.95f);
        REQUIRE(coins[i].velocity.y == Approx(20.0f * pace));
    }
    REQUIRE(coins[8].name == "COIN_SILVER");
    REQUIRE(coins[8].value == 1000);
    REQUIRE(coins[9].name == "COIN_SILVER");
    // The bronze go out evenly round the circle: the first a sixteenth of a turn from the
    // back, the fifth a sixteenth past the front, and no two the same way.
    const auto heading = [](const SpewedCoin& coin) {
        return std::atan2(coin.velocity.x, coin.velocity.z);
    };
    REQUIRE(heading(coins[0]) == Approx(-kPi + kPi / 8.0f).margin(0.001f));
    REQUIRE(heading(coins[4]) == Approx(kPi / 8.0f).margin(0.001f));
    REQUIRE(heading(coins[8]) == Approx(-kPi / 2.0f).margin(0.001f));
    REQUIRE(heading(coins[9]) == Approx(kPi / 2.0f).margin(0.001f));
    // A narrow arc ahead keeps them ahead; no players, no coins; never more than there is
    // room for.
    for (const SpewedCoin& coin : BossCoins::spray(1, 1, throwUp, kPi / 6.0f, random)) {
        REQUIRE(std::abs(heading(coin)) <= kPi / 6.0f);
    }
    REQUIRE(BossCoins::spray(7, 0, throwUp, kPi, random).empty());
    REQUIRE(BossCoins::spray(3, 4, throwUp, kPi, random).size() == 20);
    REQUIRE(BossCoins::spray(7, 8, throwUp, kPi, random).size() == BossCoins::kMost);
    REQUIRE(BossCoins::yawed(Vec3{0.0f, 0.0f, 1.0f}, kPi / 2.0f).x == Approx(1.0f));
}

} // namespace
