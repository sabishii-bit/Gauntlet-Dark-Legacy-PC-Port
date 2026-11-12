#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerPowerups.h"
#include "game/party/PowerupUpdate.h"
#include "game/players/MikeyDecoy.h"
#include "game/players/PowerupEffects.h"

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
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Paused);
    CHECK(inventory.powerups[0].strength == 10);
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    CHECK(inventory.powerups[0].strength == 8);
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::BossFight);
    CHECK(inventory.powerups[0].strength == 2);
    CHECK(inventory.powerups[1].strength == 10);
    CHECK(inventory.powerups[2].strength == -1);
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    CHECK_FALSE(inventory.powerups[0].held());
    CHECK(inventory.powerups[2].charge == 3);
}

TEST_CASE("the clock runs apart from what is worn: a boss fight spends three seconds a second, "
          "a level one, a pause none, and only the standing players' timed slots",
          "[game][items][powerups]") {
    CHECK(PlayerPowerups::rateOf(PlayerPowerups::Clock::Paused) == 0);
    CHECK(PlayerPowerups::rateOf(PlayerPowerups::Clock::Level) == 1);
    CHECK(PlayerPowerups::rateOf(PlayerPowerups::Clock::BossFight) ==
          PlayerPowerups::kBossFightRate);
    CHECK(PlayerPowerups::kBossFightRate == 3);
    std::array<PlayerRuntime, 2> players;
    players[1].life = PlayerLife::Dying;
    for (auto& player : players) {
        player.actor.save().progress().inventory.powerups[0] = {10, 5, 0, 1, true};
    }
    const auto left = [&](usize player) {
        return players[player].actor.save().progress().inventory.powerups[0].strength;
    };
    // Wearing spends nothing of a timed powerup, at whatever rate.
    PlayerPowerups::wear(players, 2, PlayerPowerups::Clock::BossFight);
    CHECK(left(0) == 10);
    PlayerPowerups::runClock(players, 2, PlayerPowerups::Clock::Paused);
    CHECK(left(0) == 10);
    PlayerPowerups::runClock(players, 2, PlayerPowerups::Clock::Level);
    CHECK(left(0) == 8);
    PlayerPowerups::runClock(players, 2, PlayerPowerups::Clock::BossFight);
    CHECK(left(0) == 2);
    CHECK(left(1) == 10); // the fallen keep theirs
}

TEST_CASE("a powerup is worn through the update its time runs out in, its ending heard on the "
          "next",
          "[game][items][powerups][endings]") {
    std::array<PlayerRuntime, 1> players;
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 0.5f);
    // The scene's update: what is worn, the party's turn, then the clock.
    CHECK(PlayerPowerups::wear(players, 0.5f, PlayerPowerups::Clock::Level).empty());
    CHECK(PowerupEffects::of(inventory).levitating());
    PlayerPowerups::runClock(players, 0.5f, PlayerPowerups::Clock::Level);
    CHECK_FALSE(inventory.powerups[0].held());
    const auto endings = PlayerPowerups::wear(players, 0.5f, PlayerPowerups::Clock::Level);
    REQUIRE(endings.size() == 1);
    CHECK(endings[0].sound == "S_LEVITATEDOWN");
}

TEST_CASE("a decoy let loose spends its slot at the clock it is worn by",
          "[game][items][powerups][mikey]") {
    for (const auto clock : {PlayerPowerups::Clock::Paused, PlayerPowerups::Clock::Level,
                             PlayerPowerups::Clock::BossFight}) {
        CAPTURE(static_cast<s32>(clock));
        std::array<PlayerRuntime, 1> players;
        auto& inventory = players[0].actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 10);
        inventory.powerups[0].on = true;
        constexpr f32 kFrame = 1.0f / MikeyDecoy::kFramesPerSecond;
        PlayerPowerups::wear(players, kFrame, clock);
        CHECK(players[0].mikey.shown());
        CHECK(inventory.powerups[0].strength ==
              Approx(10 - PlayerPowerups::rateOf(clock) * kFrame));
    }
}

TEST_CASE("activated turbo refills once while activated permanent turbo replenishes",
          "[game][items][powerups][turbo-activation]") {
    std::array<PlayerRuntime, 1> players;
    auto& inventory = players[0].actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kTurbo, 0, 1);
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 0);
    CHECK(inventory.powerupCount() == 1);
    inventory.powerups[0].on = true;
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(inventory.powerupCount() == 0);
    REQUIRE(players[0].turbo.spend(40));
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 60);
    inventory.addPowerup(powerup::kSpecial, powerup::kTurbo, 0, -1);
    CHECK_FALSE(inventory.powerups[0].on);
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 60);
    inventory.powerups[0].on = true;
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(inventory.powerupCount() == 1);
    REQUIRE(players[0].turbo.spend(40));
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
}

TEST_CASE("saved enabled Turbo Boost keeps its activation when loaded",
          "[powerups][turbo-activation][save]") {
    const auto loaded = CharacterSave::fromJson(R"({"version":1,"name":"OLD","character":0,
        "classes":{"WAR":{"inventory":{"powerups":[
            {"strength":1,"kind":9,"charge":0,"flags":524288,"on":true}]}}}})");
    REQUIRE(loaded.progress().inventory.powerups[0].on);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, loaded, nullptr, Vec3{0}, 0);
    test::powerupUpdate(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(players[0].actor.save().progress().inventory.powerupCount() == 0);
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
    test::powerupUpdate(players, 1, PlayerPowerups::Clock::Level);
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    players[0].life = PlayerLife::Standing;
    CHECK(PlayerPowerups::timeStopped(players));
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
}

TEST_CASE("coin challenges disable Stop Time without consuming any participant's inventory",
          "[powerups][stop-time][secret]") {
    std::array<PlayerRuntime, 3> players;
    players[1].life = PlayerLife::Dying;
    players[2].life = PlayerLife::InTower;
    for (auto& player : players) {
        auto& inventory = player.actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 45);
        inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 60);
    }
    REQUIRE(PlayerPowerups::timeStopped(players));
    PlayerPowerups::restrictToChallenge(players);
    CHECK_FALSE(PlayerPowerups::timeStopped(players));
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    for (auto& player : players) {
        auto& inventory = player.actor.save().progress().inventory;
        CHECK(inventory.powerupCount() == 2);
        CHECK_FALSE(inventory.powerups[0].on);
        CHECK(inventory.powerups[0].strength == 45);
        CHECK(inventory.powerups[1].on);
    }
    // It remains usable after returning to ordinary play, not silently discarded.
    auto& carried = players[0].actor.save().progress().inventory;
    carried.powerups[0].on = true;
    CHECK(PlayerPowerups::timeStopped(players));
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    CHECK(carried.powerups[0].strength == 43);
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
    test::powerupUpdate(players, 2, PlayerPowerups::Clock::Level);
    CHECK(PlayerPowerups::enemyShrinkers(players) == 1); // the timed one has run out
}

TEST_CASE("a special wearing off, or switched off, is heard once against the flags worn last "
          "update",
          "[powerups][endings]") {
    std::array<PlayerRuntime, 3> players;
    auto& first = players[0].actor.save().progress().inventory;
    auto& second = players[1].actor.save().progress().inventory;
    players[2].actor.save().character = 12; // an ogre, whose progress is its own class's
    auto& ogre = players[2].actor.save().progress().inventory;
    first.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 1);
    second.addPowerup(powerup::kSpecial, powerup::kGrowth | powerup::kPojo, 0, -1);
    ogre.addPowerup(powerup::kSpecial, powerup::kGrowth | powerup::kPojo, 0, -1);
    // Coming on is no ending.
    CHECK(test::powerupUpdate(players, 0.5f, PlayerPowerups::Clock::Level).empty());
    CHECK(players[0].wornSpecial == powerup::kLevitation);
    CHECK(players[1].wornSpecial == (powerup::kGrowth | powerup::kPojo));
    CHECK(players[2].wornSpecial == (powerup::kGrowth | powerup::kPojo));
    // Pojo's for good is switched off, and the wings' last half second runs out in this
    // update's clock: worn through it, they are heard ending on the next.
    second.powerups[0].on = false;
    const auto endings = test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level);
    REQUIRE(endings.size() == 2);
    CHECK(endings[0].partyIndex == 1);
    CHECK(endings[0].sound == "S_UNPOJO");
    CHECK(endings[1].partyIndex == 1);
    CHECK(endings[1].sound == "S_UNGROW");
    const auto wings = test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level);
    REQUIRE(wings.size() == 1);
    CHECK(wings[0].partyIndex == 0);
    CHECK(wings[0].sound == "S_LEVITATEDOWN");
    CHECK(test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level).empty());
    // An ogre never goes back to a plain size, so its growth ends unheard.
    ogre.powerups[0].on = false;
    const auto unheard = test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level);
    REQUIRE(unheard.size() == 1);
    CHECK(unheard[0].partyIndex == 2);
    CHECK(unheard[0].sound == "S_UNPOJO");
    // The fallen keep their flags for when they stand again.
    second.powerups[0].on = true;
    test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level);
    players[1].life = PlayerLife::InTower;
    second.powerups[0].on = false;
    CHECK(test::powerupUpdate(players, 1.0f, PlayerPowerups::Clock::Level).empty());
    CHECK(players[1].wornSpecial == (powerup::kGrowth | powerup::kPojo));
}
} // namespace
