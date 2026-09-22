#pragma once

#include <string_view>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"

namespace gdl::game {

/** The kinds of thing a character can pick up, by the original's item subtypes. */
enum class ItemKind : u8 {
    Gold = 1,
    Keys = 2,
    Food = 3,
    Potion = 4,
    WeaponPowerup = 5,
    ArmorPowerup = 6,
    SpeedPowerup = 7,
    MagicPowerup = 8,
    SpecialPowerup = 9,
    Runestone = 10,
    Legend = 13,     ///< an item of legend, for the boss of the realm its amount names
    Scroll = 14,     ///< a page of the level's scrolls, its amount the page from one
    Crystal = 15,
    GargoyleKey = 16 ///< a piece for the tower's gargoyles: the serpent's, eagle's or lion's
};

/** An item as it lies: what it is, how much of it there is, and what its record adds. */
struct ItemOffer {
    s32 kind = 0;
    s32 amount = 0;       ///< gold, keys, potions, health (bad food takes it), or a charge
    u32 flags = 0;        ///< a potion's kind, a powerup's which
    f32 strength = 0.0f;  ///< a powerup's
};

/** What came of touching an item. */
struct ItemTaking {
    enum class Outcome : u8 {
        Taken,       ///< all of it
        PartTaken,   ///< as much as there was room for; the rest lies there still
        Shown,       ///< it goes, though nothing is carried: a scroll read
        KeysFull,
        PotionsFull,
        HealthFull,
        AlreadyHeld, ///< a runestone the character has
        NotCarried   ///< nothing a character's inventory deals with
    };

    Outcome outcome = Outcome::NotCarried;
    s32 left = 0;  ///< of the item's amount, when part was taken
    s32 count = 0; ///< what the card shows was got; a scroll's page, a legend's realm
    std::string_view card;  ///< the STATIC texture of the card that slides up
    std::string_view sound; ///< from the common bank; empty when the class's voice answers
    bool ate = false;       ///< the class's eating sound plays
    bool hurt = false;      ///< or its pain, at food gone bad

    bool took() const {
        return outcome == Outcome::Taken || outcome == Outcome::PartTaken ||
               outcome == Outcome::Shown;
    }
};

/** The most health a character of `level` holds. */
s32 mostHealth(s32 level);

/**
 * Gives a touched item to a character the way the original does: gold up to 99999, keys and
 * potions while there is room (a key ring too big leaves its rest lying), food unless health
 * is full (food gone bad always, down to a last point of health), powerups into their slots
 * at `powerupTime` times their strength, a runestone unless held, a legend item, a gargoyle
 * piece up to what the statues want, and a scroll to read (nothing kept).
 */
ItemTaking takeItem(CharacterSave& save, const ItemOffer& offer, f32 powerupTime = 1.0f);

} // namespace gdl::game
