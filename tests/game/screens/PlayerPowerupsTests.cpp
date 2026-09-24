#include <array>

#include <catch2/catch_test_macros.hpp>

#include "game/players/PowerupEffects.h"
#include "game/screens/PlayerPowerups.h"

namespace {
using namespace gdl;
using namespace gdl::game;
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
} // namespace
