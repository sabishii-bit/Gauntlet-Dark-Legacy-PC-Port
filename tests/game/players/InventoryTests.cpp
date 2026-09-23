#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "game/players/Inventory.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("an inventory holds nine keys and nine potions at most", "[game][players][inventory]") {
    Inventory inventory;
    REQUIRE(inventory.nextPotion() == 0);
    REQUIRE(inventory.addKeys(3) == 3);
    REQUIRE(inventory.addKeys(-2) == 0);
    REQUIRE(inventory.addKeys(8) == 6); // only room for six more
    REQUIRE(inventory.keys == Inventory::kMostKeys);
    REQUIRE(inventory.addKeys(1) == 0);

    REQUIRE(inventory.addPotions(2, 1) == 1);
    REQUIRE(inventory.addPotions(4, 3) == 3);
    REQUIRE(inventory.nextPotion() == 4); // the last taken is the next thrown
    REQUIRE(inventory.addPotions(1, 9) == 5);
    REQUIRE(inventory.potions.size() == 9);
    REQUIRE(inventory.addPotions(3, 1) == 0);
    REQUIRE(inventory.nextPotion() == 1);
}

TEST_CASE("powerups renew the one held, fill a free slot, or push out the weakest",
          "[game][players][inventory]") {
    Inventory inventory;
    REQUIRE(inventory.powerupCount() == 0);
    REQUIRE(inventory.powerup(5, 0xF) == nullptr);
    inventory.addPowerup(5, 0x2, 0.0f, 30.0f);
    REQUIRE(inventory.powerupCount() == 1);
    const PowerupSlot* fire = inventory.powerup(5, 0x2);
    REQUIRE(fire != nullptr);
    REQUIRE(fire->strength == 30.0f);
    REQUIRE(inventory.powerup(5, 0x4) == nullptr);
    REQUIRE(inventory.powerup(6, 0x2) == nullptr);
    // The same again adds half its strength and all its charge.
    inventory.addPowerup(5, 0x2, 3.0f, 30.0f);
    REQUIRE(inventory.powerupCount() == 1);
    REQUIRE(fire->strength == 45.0f);
    REQUIRE(fire->charge == 3.0f);
    // A strength under none makes it one for good, which more of the same no longer adds to.
    inventory.addPowerup(5, 0x2, 0.0f, -1.0f);
    REQUIRE(fire->strength == -1.0f);
    inventory.addPowerup(5, 0x2, 0.0f, 30.0f);
    REQUIRE(fire->strength == -1.0f);
    // Ten more fill the other slots; an eleventh pushes out the weakest of those, never the
    // one held for good.
    for (std::int32_t i = 0; i < 10; ++i) {
        inventory.addPowerup(9, 1U << static_cast<std::uint32_t>(i), 0.0f,
                             10.0f + static_cast<float>(i));
    }
    REQUIRE(inventory.powerupCount() == Inventory::kPowerupSlots);
    inventory.addPowerup(7, 0x1, 0.0f, 99.0f);
    REQUIRE(inventory.powerupCount() == Inventory::kPowerupSlots);
    REQUIRE(inventory.powerup(9, 0x1) == nullptr); // the weakest, at ten
    REQUIRE(inventory.powerup(9, 0x2) != nullptr);
    REQUIRE(inventory.powerup(7, 0x1)->strength == 99.0f);
    REQUIRE(inventory.powerup(5, 0x2) != nullptr);
    REQUIRE(inventory == inventory);
    REQUIRE_FALSE(inventory == Inventory{});
}

} // namespace
