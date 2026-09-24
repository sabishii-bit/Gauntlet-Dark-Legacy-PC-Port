
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/players/PickupVoices.h"
#include "game/players/Progression.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Outcome = ItemTaking::Outcome;

ItemOffer offer(ItemKind kind, s32 amount, u32 flags = 0, f32 strength = 0.0f) {
    return ItemOffer{static_cast<s32>(kind), amount, flags, strength};
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
    for (s32 i = 0; i < 30; ++i) {
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
    CHECK(taking.sound.empty());
    CHECK_FALSE(taking.ate);
    CHECK_FALSE(taking.hurt);
    CHECK(taking.message == -1);
}

TEST_CASE("every powerup pickup selects the retail message independently of its sound",
          "[game][players][items]") {
    struct Expected {
        s32 kind;
        u32 flags;
        s32 message;
        std::string_view sound = "S_PICKUPSPECIAL";
    };
    const std::array cases{
        Expected{5, 0x80000, 37}, Expected{5, 0x400000, 47}, Expected{5, 0x200000, 38},
        Expected{5, 0x100000, 48}, Expected{5, 0x10000000, 86}, Expected{5, 0x20000000, 87},
        Expected{5, 1, 40}, Expected{5, 2, 41}, Expected{5, 3, 42}, Expected{5, 4, 43},
        Expected{6, 0x100000, 54}, Expected{6, 0x10000, 35}, Expected{6, 0x80000, 49},
        Expected{6, 0x20000, 52}, Expected{6, 0x200000, 91, "S_PICKUPSHIELD"},
        Expected{6, 0x400000, 92}, Expected{6, 0x2000, 132}, Expected{7, 0, 32}, Expected{8, 0, 33},
        Expected{9, 4, 36}, Expected{9, 2, 39}, Expected{9, 8, 51},
        Expected{9, 1, 53, "S_LEVITATEUP"}, Expected{9, 0x10, 81}, Expected{9, 0x20, 82},
        Expected{9, 0x40, 83}, Expected{9, 0x80, 84}, Expected{9, 0x100, 88, "S_GROW"},
        Expected{9, 0x200, 89, "S_SHRINK"}, Expected{9, 0x400, 93, "S_POJO"},
        Expected{9, 0x2000, 98}, Expected{9, 0x1000, 99}, Expected{9, 0x8000, 100},
        Expected{9, 0x4000, 100}, Expected{9, 0x80000, 113}, Expected{9, 0x100000, 148},
        Expected{9, 0x200000, 149}, Expected{9, 0x400000, 150},
        // Ordered selection, exact low-nibble comparison, and no invented unknown cue.
        Expected{5, 0x480004, 37}, Expected{5, 7, -1}, Expected{9, 0x101, 53, "S_LEVITATEUP"},
        Expected{9, 0x10000, -1}, Expected{9, 0, -1}};
    for (const Expected& expected : cases) {
        CAPTURE(expected.kind, expected.flags);
        CharacterSave save;
        const ItemTaking taking = takeItem(save, ItemOffer{expected.kind, 30, expected.flags, 1});
        REQUIRE(taking.took());
        CHECK(taking.message == expected.message);
        CHECK(taking.sound == expected.sound);
    }
}

TEST_CASE("food voices distinguish class lines, archer fruit, poison and Pojo", "[game][items]") {
    CHECK(PickupVoices::foodChoice(0, "MEAT", false, false, false).sound == "S_WAREATSFX");
    CHECK(PickupVoices::foodChoice(0, "MEAT", false, false, true).sound == "S_WAREAT");
    CHECK(PickupVoices::foodChoice(1, "APPLE", true, false, false).sound == "S_VALPOISON");
    CHECK(PickupVoices::foodChoice(3, "MEAT", false, false, true).sound == "S_ARCEAT1");
    CHECK(PickupVoices::foodChoice(3, "APPLE", false, false, true).sound == "S_ARCEAT2");
    CHECK(PickupVoices::foodChoice(3, "BANANA", false, false, true).sound == "S_ARCEAT3");
    CHECK(PickupVoices::foodChoice(3, "PINEAPPLE", false, false, true).sound == "S_ARCEAT4");
    CHECK(PickupVoices::foodChoice(11, "APPLE", false, false, true).sound == "S_ARCEAT2");
    CHECK(PickupVoices::foodChoice(0, "MEAT", false, true, true).sound.empty());
    CHECK(PickupVoices::foodChoice(0, "MEAT", false, true, false).sound == "S_POJOEATSFX");
    CHECK(PickupVoices::foodChoice(0, "MEAT", true, true, false).sound == "S_POJOPOISON");
    CHECK(PickupVoices::foodChoice(0, "MEAT", true, true, false).common);
    CHECK(PickupVoices::bonusGold(0, 50) == "S_PKUPBRONZE1");
    CHECK(PickupVoices::bonusGold(1, 100) == "S_PKUPSILVER2");
    CHECK(PickupVoices::bonusGold(3, 500) == "S_PKUPGOLD4");
    CHECK(PickupVoices::bonusGold(2, 25) == "S_PKUPGOLD3");
    CharacterSave save;
    save.progress().health = 100;
    CHECK(takeItem(save, offer(ItemKind::Food, 100)).message == 15);
    CHECK(takeItem(save, offer(ItemKind::Food, 50)).message == 16);
    CHECK(takeItem(save, offer(ItemKind::Food, -50)).message == 28);
    CHECK(takeItem(save, offer(ItemKind::Food, 1)).message == -1);
}

} // namespace
