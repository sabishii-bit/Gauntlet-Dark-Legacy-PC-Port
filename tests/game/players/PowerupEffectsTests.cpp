#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/players/PowerupEffects.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("X-Ray follows the equipped slot rather than merely carried inventory",
          "[game][players][powerups][xray]") {
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kXRay, 0, 30);
    REQUIRE(PowerupEffects::of(inventory).xray());
    for (auto& slot : inventory.powerups) {
        slot.on = false;
    }
    REQUIRE_FALSE(PowerupEffects::of(inventory).xray());
}

TEST_CASE("worn powerups add up to what they do", "[game][players][powerups]") {
    Inventory inventory;
    PowerupEffects effects = PowerupEffects::of(inventory);
    REQUIRE(effects.shots() == 1);
    REQUIRE_FALSE(effects.invisible());
    REQUIRE_FALSE(effects.grown());
    REQUIRE(effects.bodyAlpha(0.3f) == 1.0f);
    REQUIRE(effects.paceAdd == 0.0f);

    inventory.addPowerup(powerup::kWeapon, powerup::kThreeWayShot, 0.0f, 30.0f);
    inventory.addPowerup(powerup::kSpeed, 0, 2.5f, 30.0f);
    inventory.addPowerup(powerup::kMagic, 0, 40.0f, 30.0f);
    inventory.addPowerup(powerup::kSpecial, powerup::kInvisible, 0.0f, 30.0f);
    inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0.0f, 30.0f);
    inventory.addPowerup(powerup::kArmor, 0x10000, 0.0f, 30.0f);
    effects = PowerupEffects::of(inventory);
    REQUIRE(effects.shots() == 3);
    REQUIRE(effects.paceAdd == 2.5f);
    REQUIRE(effects.magicAdd == 40.0f);
    REQUIRE(effects.armor == 0x10000U);
    REQUIRE(PowerupEffects{}.magicPower(0) == 8.0f);
    REQUIRE(PowerupEffects{}.magicPower(1000) == 32.0f);
    REQUIRE(effects.magicPower(500) == Approx(20.0f + 40.0f));
    REQUIRE(effects.invisible());
    REQUIRE(effects.grown());
    // Unseen, the body shows about a third solid, wavering over each second.
    REQUIRE(effects.bodyAlpha(0.0f) == Approx(95.0f / 255.0f));
    REQUIRE(effects.bodyAlpha(0.25f) == Approx(79.0f / 255.0f));
    REQUIRE(effects.bodyAlpha(0.75f) == Approx(111.0f / 255.0f));
    // Five ways beats three; one taken off does nothing.
    inventory.addPowerup(powerup::kWeapon, powerup::kFiveWayShot, 0.0f, 30.0f);
    REQUIRE(PowerupEffects::of(inventory).shots() == 5);
    for (PowerupSlot& slot : inventory.powerups) {
        slot.on = slot.kind != powerup::kWeapon;
    }
    effects = PowerupEffects::of(inventory);
    REQUIRE(effects.shots() == 1);
    REQUIRE(effects.weapon == 0U);
    REQUIRE(effects.invisible());
    REQUIRE(inventory.powerup(powerup::kWeapon, powerup::kFiveWayShot) == nullptr);
}

TEST_CASE("a powerup is named by the first of the original's list whose flags it carries",
          "[game][players][powerups]") {
    REQUIRE(powerupTextId(9, 0x4) == "powerup.invisible");
    REQUIRE(powerupTextId(9, 0x104) == "powerup.invisible"); // the earlier entry wins
    REQUIRE(powerupTextId(5, 0x80000) == "powerup.threeWayShot");
    REQUIRE(powerupTextId(5, 0x400000) == "powerup.fiveWayShot");
    REQUIRE(powerupTextId(5, 0x1) == "powerup.weapon"); // an elemental weapon: the kind's name
    REQUIRE(powerupTextId(7, 0) == "powerup.speedBoost");
    REQUIRE(powerupTextId(8, 0x55) == "powerup.magicBoost");
    REQUIRE(powerupTextId(6, 0x10000) == "powerup.invulnerable");
    REQUIRE(powerupTextId(6, 0x40) == "powerup.armor");
    REQUIRE(powerupTextId(3, 0) == "powerup.unknown");
}

TEST_CASE("keys are spent and potions taken out one at a time", "[game][players][inventory]") {
    Inventory inventory;
    REQUIRE_FALSE(inventory.spendKey());
    inventory.addKeys(2);
    REQUIRE(inventory.spendKey());
    REQUIRE(inventory.keys == 1);
    REQUIRE(inventory.takePotion() == 0);
    inventory.addPotions(1, 1);
    inventory.addPotions(4, 1);
    REQUIRE(inventory.takePotion() == 4);
    REQUIRE(inventory.takePotion() == 1);
    REQUIRE(inventory.potions.empty());
    // Going round the held slots, either way, wrapping; nothing held is nothing to go to.
    REQUIRE(inventory.nextHeld(-1, 1) == -1);
    inventory.powerups[2] = PowerupSlot{10.0f, 9, 0.0f, 1, true};
    inventory.powerups[7] = PowerupSlot{10.0f, 9, 0.0f, 2, false};
    REQUIRE(inventory.nextHeld(-1, -1) == 7);
    REQUIRE(inventory.nextHeld(7, -1) == 2);
    REQUIRE(inventory.nextHeld(2, -1) == 7);
    REQUIRE(inventory.nextHeld(2, 1) == 7);
    REQUIRE(inventory.nextHeld(7, 1) == 2);
}

TEST_CASE("only the longest enabled amulet supplies the elemental nibble",
          "[game][items][powerups]") {
    Inventory inventory;
    inventory.addPowerup(5, 1, 0, 90);
    inventory.addPowerup(5, 2, 0, 60);
    inventory.addPowerup(5, powerup::kThreeWayShot, 0, 30);
    CHECK(PowerupEffects::of(inventory).weapon == (powerup::kThreeWayShot | 1));
    inventory.powerups[0].on = false;
    CHECK(PowerupEffects::of(inventory).weapon == (powerup::kThreeWayShot | 2));
    inventory.powerups[0].on = true;
    inventory.powerups[1].strength = 100;
    CHECK(PowerupEffects::of(inventory).weapon == (powerup::kThreeWayShot | 2));
}

TEST_CASE("charged powerups expire on the final use and disabled items cannot be spent",
          "[game][items][inventory]") {
    Inventory inventory;
    inventory.addPowerup(5, 0x100000, 2, -1);
    REQUIRE(inventory.spendPowerup(5, 0x100000));
    CHECK(inventory.powerups[0].charge == 1);
    inventory.powerups[0].on = false;
    CHECK_FALSE(inventory.spendPowerup(5, 0x100000));
    inventory.powerups[0].on = true;
    REQUIRE(inventory.spendPowerup(5, 0x100000));
    CHECK_FALSE(inventory.powerups[0].held());
    CHECK_FALSE(inventory.spendPowerup(5, 0x100000));
}
} // namespace
