
#include <catch2/catch_test_macros.hpp>

#include "game/players/ItemPickup.h"
#include "game/players/Progression.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Outcome = ItemTaking::Outcome;

ItemOffer offer(ItemKind kind, int amount, unsigned int flags = 0, float strength = 0.0f) {
    return ItemOffer{static_cast<int>(kind), amount, flags, strength};
}

TEST_CASE("gold adds up to its limit and shows as treasure or junk", "[game][players][items]") {
    CharacterSave save;
    ItemTaking taking = takeItem(save, offer(ItemKind::Gold, 200));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.took());
    REQUIRE(save.gold == 200);
    REQUIRE(taking.count == 200);
    REQUIRE(taking.card == "GOLD");
    REQUIRE(taking.sound == "S_PICKUPMAGIC");
    REQUIRE(takeItem(save, offer(ItemKind::Gold, 10)).card == "JUNK");
    save.gold = 99990;
    takeItem(save, offer(ItemKind::Gold, 500));
    REQUIRE(save.gold == 99999);
}

TEST_CASE("keys are taken while there is room, a ring leaving what does not fit",
          "[game][players][items]") {
    CharacterSave save;
    ItemTaking taking = takeItem(save, offer(ItemKind::Keys, 1));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "KEY");
    REQUIRE(taking.sound == "S_PICKUPKEY");
    REQUIRE(save.progress().inventory.keys == 1);
    REQUIRE(takeItem(save, offer(ItemKind::Keys, 3)).card == "KEY_RING");
    save.progress().inventory.keys = 7;
    taking = takeItem(save, offer(ItemKind::Keys, 3));
    REQUIRE(taking.outcome == Outcome::PartTaken);
    REQUIRE(taking.left == 1);
    REQUIRE(taking.count == 2);
    REQUIRE(save.progress().inventory.keys == 9);
    taking = takeItem(save, offer(ItemKind::Keys, 1));
    REQUIRE(taking.outcome == Outcome::KeysFull);
    REQUIRE_FALSE(taking.took());
    REQUIRE(taking.card.empty());
}

TEST_CASE("potions are kept by kind until nine are carried", "[game][players][items]") {
    CharacterSave save;
    const ItemTaking taking = takeItem(save, offer(ItemKind::Potion, 1, 2));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "MAGIC");
    REQUIRE(save.progress().inventory.nextPotion() == 2);
    save.progress().inventory.potions.assign(9, 1);
    REQUIRE(takeItem(save, offer(ItemKind::Potion, 1, 3)).outcome == Outcome::PotionsFull);
    REQUIRE(save.progress().inventory.nextPotion() == 1);
}

TEST_CASE("food heals up to the level's most, is left at full health, and bad food always bites",
          "[game][players][items]") {
    REQUIRE(mostHealth(1) == 500);
    REQUIRE(mostHealth(10) == 1400);
    REQUIRE(mostHealth(99) == 9999);
    CharacterSave save;
    REQUIRE(save.health() == 500);
    REQUIRE(takeItem(save, offer(ItemKind::Food, 100)).outcome == Outcome::HealthFull);
    save.progress().health = 300;
    ItemTaking taking = takeItem(save, offer(ItemKind::Food, 100));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(save.health() == 400);
    REQUIRE(taking.card == "MEAT");
    REQUIRE(taking.ate);
    REQUIRE(taking.sound.empty()); // the class's own eating answers
    taking = takeItem(save, offer(ItemKind::Food, 500));
    REQUIRE(save.health() == 500); // no more than the most
    REQUIRE(takeItem(save, offer(ItemKind::Food, -50)).card == "BADFRUIT");
    REQUIRE(save.health() == 450);
    taking = takeItem(save, offer(ItemKind::Food, -100));
    REQUIRE(taking.card == "BADMEAT");
    REQUIRE(taking.hurt);
    REQUIRE_FALSE(taking.ate);
    save.progress().health = 40;
    takeItem(save, offer(ItemKind::Food, -100));
    REQUIRE(save.health() == 1); // to the last point, not past it
    save.progress().health = 499;
    REQUIRE(takeItem(save, offer(ItemKind::Food, 25)).card == "FRUIT");
}

TEST_CASE("powerups go into their slots at the class's share of their strength",
          "[game][players][items]") {
    CharacterSave save;
    ItemTaking taking = takeItem(save, offer(ItemKind::SpecialPowerup, 0, 0x8000, 30.0f), 1.5f);
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "SPECIALS");
    REQUIRE(taking.sound == "S_PICKUPSPECIAL");
    const PowerupSlot* slot = save.progress().inventory.powerup(9, 0x8000);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->strength == 45.0f);
    taking = takeItem(save, offer(ItemKind::ArmorPowerup, 0, 0x200000, 20.0f));
    REQUIRE(taking.sound == "S_PICKUPSHIELD");
    // Anything else is not the inventory's to deal with.
    REQUIRE(takeItem(save, offer(ItemKind::Crystal, 4)).outcome == Outcome::NotCarried);
    REQUIRE(takeItem(save, ItemOffer{44, 0, 0, 0.0f}).outcome == Outcome::NotCarried);
}

TEST_CASE("a runestone is kept once, a legend item by its realm, and gargoyle pieces are "
          "counted up to what the statues want",
          "[game][players][items]") {
    CharacterSave save;
    Relics& relics = save.progress().relics;
    ItemTaking taking = takeItem(save, offer(ItemKind::Runestone, 7));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "RUNESTONE");
    REQUIRE(taking.sound == "S_PICKUPRUNE");
    REQUIRE(taking.count == 7);
    REQUIRE(relics.hasRune(7));
    // The same rune again is refused, and lies there still.
    taking = takeItem(save, offer(ItemKind::Runestone, 7));
    REQUIRE(taking.outcome == Outcome::AlreadyHeld);
    REQUIRE_FALSE(taking.took());
    REQUIRE(relics.runeCount() == 1);
    // A legend item: the ice axe is the mountain's, realm two.
    taking = takeItem(save, offer(ItemKind::Legend, 2));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "LEGEND");
    REQUIRE(taking.sound == "S_PICKUPMAGIC");
    REQUIRE(taking.count == 2);
    REQUIRE(relics.hasLegend(2));
    REQUIRE_FALSE(relics.hasLegend(1));
    // Gargoyle pieces: the count so far shows, and it stops at what is wanted.
    taking = takeItem(save, offer(ItemKind::GargoyleKey, 1));
    REQUIRE(taking.outcome == Outcome::Taken);
    REQUIRE(taking.card == "GOLDNICON");
    REQUIRE(taking.count == 1);
    for (int i = 0; i < 30; ++i) {
        takeItem(save, offer(ItemKind::GargoyleKey, 1));
    }
    REQUIRE(relics.gargoylePieces[1] == Relics::kGargoyleNeeded[1]);
    REQUIRE(relics.gargoyleComplete(1));
    REQUIRE_FALSE(relics.gargoyleComplete(0));
    REQUIRE(takeItem(save, offer(ItemKind::GargoyleKey, 9)).outcome == Outcome::NotCarried);
}

TEST_CASE("a scroll is read where it lies and goes, nothing kept", "[game][players][items]") {
    CharacterSave save;
    const ItemTaking taking = takeItem(save, offer(ItemKind::Scroll, 3));
    REQUIRE(taking.outcome == Outcome::Shown);
    REQUIRE(taking.took());
    REQUIRE(taking.count == 2); // the page, from nought
    REQUIRE(taking.card.empty());
    REQUIRE(save.progress().relics == Relics{});
    REQUIRE(save.progress().inventory == Inventory{});
}

} // namespace
