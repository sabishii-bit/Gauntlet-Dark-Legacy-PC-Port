#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/players/PowerupEffects.h"
#include "game/screens/PlayerPowerups.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
TEST_CASE("powerup clocks pause and boss combat spends three seconds per second",
          "[game][items][powerups]") {
    std::array<PlayerRuntime, 1> players;
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.powerups[0] = {10, 5, 0, 1, true};
    inventory.powerups[1] = {10, 5, 0, 2, false};
    inventory.powerups[2] = {-1, 5, 3, 0x100000, true};
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::Paused);
    CHECK(inventory.powerups[0].strength == 10);
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::Level);
    CHECK(inventory.powerups[0].strength == 8);
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::BossFight);
    CHECK(inventory.powerups[0].strength == 2);
    CHECK(inventory.powerups[1].strength == 10);
    CHECK(inventory.powerups[2].strength == -1);
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::Level);
    CHECK_FALSE(inventory.powerups[0].held());
    CHECK(inventory.powerups[2].charge == 3);
}
TEST_CASE("turbo pickups refill once while permanent turbo replenishes",
          "[game][items][powerups]") {
    std::array<PlayerRuntime, 1> players;
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kTurbo, 0, 1);
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(inventory.powerupCount() == 0);
    REQUIRE(players[0].turbo.spend(40));
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 60);
    inventory.addPowerup(powerup::kSpecial, powerup::kTurbo, 0, -1);
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(inventory.powerupCount() == 1);
}
TEST_CASE("Stop Time is shared only by standing players with a working item",
          "[powerups][stop-time]") {
    std::array<PlayerRuntime, 3> players;
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    auto& first = players[0].actor.save().progress().inventory;
    auto& second = players[1].actor.save().progress().inventory;
    first.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 2);
    CHECK(PlayerPowerups::timeStopped(players));
    first.powerups[0].on = false;
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    first.powerups[0].on = true;
    players[0].life = PlayerLife::Dying;
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    second.addPowerup(powerup::kSpecial, powerup::kStopTime | powerup::kInvisible, 0, 1);
    CHECK(PlayerPowerups::timeStopped(players));
    PlayerPowerups::update(players, 1, PlayerPowerups::Clock::Level);
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    players[0].life = PlayerLife::Standing;
    CHECK(PlayerPowerups::timeStopped(players));
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::Level);
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
}

TEST_CASE("the shrinkers counted are the standing players with a working slot switched on, and "
          "the scale they leave is two thirds a wearer outside a boss's arena",
          "[powerups][shrink]") {
    std::array<PlayerRuntime, 3> players;
    CHECK(PlayerPowerups::enemyShrinkers(players) == 0);
    CHECK(PlayerPowerups::enemyShrink(players, false) == 1.0f);
    auto& first = players[0].actor.save().progress().inventory;
    auto& second = players[1].actor.save().progress().inventory;
    first.addPowerup(powerup::kSpecial, powerup::kEnemyShrink, 0, 2);
    CHECK(PlayerPowerups::enemyShrinkers(players) == 1);
    CHECK(PlayerPowerups::enemyShrink(players, false) == Approx(0.667f));
    CHECK(PlayerPowerups::enemyShrink(players, true) == 1.0f);
    first.powerups[0].on = false;
    CHECK(PlayerPowerups::enemyShrinkers(players) == 0);
    first.powerups[0].on = true;
    second.addPowerup(powerup::kSpecial, powerup::kEnemyShrink | powerup::kGrowth, 0, -1);
    CHECK(PlayerPowerups::enemyShrinkers(players) == 2);
    CHECK(PlayerPowerups::enemyShrink(players, false) == Approx(0.667f * 0.667f));
    players[1].life = PlayerLife::Dying;
    CHECK(PlayerPowerups::enemyShrinkers(players) == 1);
    players[1].life = PlayerLife::Standing;
    PlayerPowerups::update(players, 2, PlayerPowerups::Clock::Level);
    CHECK(PlayerPowerups::enemyShrinkers(players) == 1); // the timed one has run out
}
} // namespace
