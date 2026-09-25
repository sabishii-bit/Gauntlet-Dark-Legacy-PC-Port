#include <array>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/SecretChallenge.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("secret worlds award their authored classes, not realm-order guesses", "[secret]") {
    const std::array<s32, 9> classes{10, 11, 9, 8, 16, 12, 15, 14, 13};
    SecretChallenge challenge;
    for (usize i = 0; i < classes.size(); ++i) {
        REQUIRE(challenge.begin(static_cast<s32>(i), 30, std::array<usize, 1>{5}));
        CHECK(challenge.rewardClass() == classes[i]);
        CHECK(challenge.rewardMask() == (1U << (classes[i] - 8)));
    }
    CHECK_FALSE(challenge.begin(-1, 30, {}));
    CHECK_FALSE(challenge.begin(9, 30, {}));
    CHECK_FALSE(challenge.begin(0, 0, {}));
    CHECK(challenge.state() == SecretChallenge::State::Inactive);
}

TEST_CASE("coin goals count distinct placed coins, not denominations or bonus drops", "[secret]") {
    SecretChallenge challenge;
    REQUIRE(challenge.begin(3, 45, std::array<usize, 3>{2, 4, 7}));
    CHECK(challenge.duration() == Approx(45.99f));
    CHECK_FALSE(challenge.collect(123));
    CHECK_FALSE(challenge.collect(2));
    CHECK_FALSE(challenge.collect(2));
    CHECK(challenge.coinsLeft() == 2);
    CHECK_FALSE(challenge.collect(7));
    CHECK(challenge.collect(4));
    CHECK(challenge.state() == SecretChallenge::State::Won);
    CHECK_FALSE(challenge.collect(4));
    CHECK(challenge.remaining() == 1);
    CHECK(challenge.step(100, true).empty()); // unlock scroll holds the return clock
    CHECK(challenge.state() == SecretChallenge::State::Won);
    CHECK(challenge.step(1).empty()); // no failure countdown after victory
    CHECK(challenge.state() == SecretChallenge::State::Returning);
}

TEST_CASE("challenge clock pauses and expires without granting an unlock", "[secret]") {
    SecretChallenge challenge;
    REQUIRE(challenge.begin(0, 9, std::array<usize, 1>{1}));
    CHECK(challenge.step(2, true).empty());
    CHECK(challenge.remaining() == Approx(9.99f));
    CHECK(challenge.step(1) == std::vector<s32>{8});
    CHECK(challenge.step(4) == std::vector<s32>{7, 6, 5, 4});
    CHECK(challenge.step(-1).empty());
    CHECK(challenge.step(std::numeric_limits<f32>::quiet_NaN()).empty());
    CHECK(challenge.step(10) == std::vector<s32>{3, 2, 1, 0});
    CHECK(challenge.state() == SecretChallenge::State::Returning);
    CHECK_FALSE(challenge.collect(1));
    CHECK(challenge.coinsLeft() == 1);
    challenge.clear();
    CHECK(challenge.state() == SecretChallenge::State::Inactive);
    REQUIRE(challenge.begin(0, 1, {}));
    CHECK_FALSE(challenge.collect(0)); // empty/missing coins cannot auto-award a class
}
} // namespace
