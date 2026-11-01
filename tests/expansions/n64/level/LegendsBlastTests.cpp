#include <limits>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsBlast.h"

namespace {
using namespace gdl;
using namespace gdl::game;

LegendsFixtureEvent blast(bool poison = false) {
    LegendsFixtureEvent result;
    result.kind = poison ? LegendsFixtureEvent::Kind::Poison : LegendsFixtureEvent::Kind::Explosion;
    result.position = {0, 2, 0};
    result.radius = poison ? 4.5f : 5;
    result.damage = poison ? 30 : 40;
    result.seconds = 2;
    result.shrinkFraction = poison ? .1f : .2f;
    result.noFalloff = poison;
    return result;
}
} // namespace

TEST_CASE("Native barrel blast radius follows the active tree lifetime", "[legends][blasts]") {
    LegendsBlast state{blast()};
    REQUIRE(state.step(0)->radius == 0);
    CHECK(state.step(.1f)->radius == Catch::Approx(2.5f));
    CHECK(state.step(.1f)->radius == Catch::Approx(5));
    CHECK(state.step(1.4f)->radius == Catch::Approx(5));
    CHECK(state.step(.2f)->radius == Catch::Approx(2.5f));
    CHECK_FALSE(state.step(1));
    CHECK_FALSE(state.alive());
    CHECK_FALSE(state.step(0));
}

TEST_CASE("Native barrel hits each player once and can reach late arrivals", "[legends][blasts]") {
    LegendsBlast state{blast()};
    const LegendsPortalVisitor first{0, {0, 0, 0}, true, .75f, 4};
    CHECK_FALSE(state.touch(first));
    REQUIRE(state.step(.2f));
    REQUIRE(state.touch(first));
    CHECK_FALSE(state.touch(first));
    REQUIRE(state.step(.2f));
    CHECK_FALSE(state.touch(first));
    CHECK_FALSE(state.touch({1, {50, 0, 0}, true, .75f, 4}));
    CHECK_FALSE(state.touch({1, {0, 50, 0}, true, .75f, 4}));
    CHECK_FALSE(state.touch({1, {0, 0, 0}, false, .75f, 4}));
    CHECK(state.touch({1, {0, 0, 0}, true, .75f, 4}));
    CHECK_FALSE(state.touch({1, {0, 0, 0}, true, .75f, 4}));
    CHECK(state.touch({3, {0, 0, 0}, true, .75f, 4}));
    CHECK_FALSE(state.touch({4, {0, 0, 0}, true, .75f, 4}));
    state.reachedOpponents().push_back(10);
    state.reachedFixtures().push_back(20);
    REQUIRE(state.step(.2f));
    CHECK(state.reachedOpponents() == std::vector<s32>{10});
    CHECK(state.reachedFixtures() == std::vector<s32>{20});
}

TEST_CASE("Native fire damage attenuates its physical portion but poison does not",
          "[legends][blasts]") {
    const LegendsBlast fire{blast()};
    CHECK(fire.damageAt({0, 2, 0}) == 39); // round40*.66 + round40*.33
    CHECK(fire.physicalDamageAt({0, 2, 0}) == 26);
    CHECK(fire.damageAt({5, 2, 0}) == 15); // physical minimum5%, elemental13
    LegendsBlast poison{blast(true)};
    CHECK(poison.damageAt({0, 2, 0}) == 30);
    CHECK(poison.physicalDamageAt({0, 2, 0}) == 3);
    CHECK(poison.damageAt({4.5f, 2, 0}) == 30);
    REQUIRE(poison.step(.2f));
    CHECK(poison.touch({0, {0, 0, 0}, true, .75f, 4}) == 30);
    REQUIRE(poison.step(.5f));
    CHECK_FALSE(poison.touch({0, {0, 0, 0}, true, .75f, 4}));
    const LegendsBlast scaled{blast(true), 2};
    CHECK(scaled.damageAt({0, 2, 0}) == 60);
}

TEST_CASE("Native blast rejects invalid timing without resetting hit admission",
          "[legends][blasts]") {
    auto invalid = blast();
    invalid.seconds = 0;
    CHECK_THROWS_AS(LegendsBlast{invalid}, std::invalid_argument);
    invalid = blast();
    invalid.radius = std::numeric_limits<f32>::quiet_NaN();
    CHECK_THROWS_AS(LegendsBlast{invalid}, std::invalid_argument);
    LegendsBlast state{blast()};
    REQUIRE(state.step(.2f));
    REQUIRE(state.touch({0, {0, 0, 0}, true, .75f, 4}));
    CHECK_THROWS_AS(state.step(-1), std::invalid_argument);
    CHECK_THROWS_AS(state.step(std::numeric_limits<f32>::infinity()), std::invalid_argument);
    CHECK_FALSE(state.touch({0, {0, 0, 0}, true, .75f, 4}));
}
