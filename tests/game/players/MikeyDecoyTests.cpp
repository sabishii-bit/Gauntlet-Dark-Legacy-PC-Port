#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/players/MikeyDecoy.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerPowerups.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
constexpr f32 kFrame = 1.0f / MikeyDecoy::kFramesPerSecond;

TEST_CASE("Mikey is manually dropped and does not follow or spend its item while deployed",
          "[mikey][powerups]") {
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 7, 10);
    auto& slot = inventory.powerups[0];
    CHECK_FALSE(slot.on);
    MikeyDecoy decoy;
    decoy.update(kFrame, inventory, Vec3{1, 3, 5});
    REQUIRE_FALSE(decoy.shown());
    slot.on = true;
    decoy.update(kFrame, inventory, Vec3{1, 3, 5});
    CHECK(decoy.shown());
    CHECK_FALSE(decoy.target()); // State two is drawn, but not yet a lure.
    CHECK_FALSE(slot.on);
    CHECK(slot.strength == Approx(10 - kFrame));
    CHECK(slot.charge == 7);
    decoy.update(kFrame, inventory, Vec3{50, 9, -10});
    REQUIRE(decoy.target());
    CHECK(*decoy.target() == Vec3{1, 3, 5});
    inventory.advance(3);
    CHECK(slot.strength == Approx(10 - kFrame));
    decoy.update(2, inventory, Vec3{50});
    CHECK(decoy.takeSparkles() == 5); // PlayerProcessMikeyPUP states 10,20,30,40,50.
    CHECK(decoy.takeSparkles() == 0);
}

TEST_CASE("Mikey runs its 300-frame state machine at thirty Hz independently of render rate",
          "[mikey][powerups]") {
    const s32 rate = GENERATE(30, 60, 120);
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, -1);
    inventory.powerups[0].on = true;
    MikeyDecoy decoy;
    decoy.update(kFrame, inventory, Vec3{0});
    for (s32 frame = 0; frame < rate * 9; ++frame) {
        decoy.update(1.0f / static_cast<f32>(rate), inventory, Vec3{1});
    }
    CHECK(decoy.state() == 272);
    CHECK(decoy.takeSparkles() == 5);
    decoy.update(kFrame * 28, inventory, Vec3{1});
    CHECK(decoy.state() == 300);
    REQUIRE(decoy.target());
    decoy.update(kFrame, inventory, Vec3{1});
    CHECK_FALSE(decoy.shown());
    CHECK_FALSE(decoy.target());
    decoy.update(1, inventory, Vec3{1});
    CHECK_FALSE(decoy.shown()); // The retained inventory item does not automatically redeploy.
}

TEST_CASE("Mikey's next selector activation dismisses it and a later one drops a fresh lure",
          "[mikey][powerups]") {
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, -1);
    auto& slot = inventory.powerups[0];
    slot.on = true;
    MikeyDecoy decoy;
    decoy.update(1, inventory, Vec3{0});
    const u32 generation = decoy.generation();
    slot.on = true;
    decoy.update(kFrame, inventory, Vec3{1});
    CHECK(decoy.state() == 300);
    decoy.update(kFrame, inventory, Vec3{1});
    CHECK_FALSE(decoy.shown());
    CHECK_FALSE(slot.on);
    slot.on = true;
    decoy.update(2 * kFrame, inventory, Vec3{1});
    REQUIRE(decoy.target());
    CHECK(*decoy.target() == Vec3{1});
    CHECK(decoy.generation() == generation + 1);
    decoy.clear();
    CHECK_FALSE(decoy.shown());
    CHECK(decoy.takeSparkles() == 0);
}

TEST_CASE("Mikey's lure reaches the swarm snapshot without replacing the player's body",
          "[mikey][powerups]") {
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    auto& inventory = player.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 10);
    inventory.powerups[0].on = true;
    PlayerPowerups::update(players, 2 * kFrame, PlayerPowerups::Clock::Paused);
    CHECK(inventory.powerups[0].strength == 10); // Tower freezes item time, not the decoy.
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 1);
    CHECK(views[0].position == player.actor.position());
    REQUIRE(views[0].decoy);
    CHECK(*views[0].decoy == player.actor.followPoint());
    player.life = PlayerLife::Dying;
    const s32 beforeDeath = player.mikey.state();
    PlayerPowerups::update(players, kFrame, PlayerPowerups::Clock::Level);
    CHECK(player.mikey.shown());
    CHECK(player.mikey.state() == beforeDeath + 1);
    CHECK(LevelOpponents::enemyViews(players)[0].hidden);
    player.life = PlayerLife::InTower;
    PlayerPowerups::update(players, kFrame, PlayerPowerups::Clock::Level);
    CHECK_FALSE(player.mikey.shown());
    CHECK_FALSE(LevelOpponents::enemyViews(players)[0].decoy);
}
} // namespace
