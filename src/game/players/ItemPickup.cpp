#include "game/players/ItemPickup.h"

#include <algorithm>
#include <cstdint>

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::int32_t kMostGold = 99999;
constexpr std::int32_t kBaseHealth = 500;
constexpr std::int32_t kHealthPerLevel = 100;
constexpr std::int32_t kHealthLimit = 9999;
constexpr std::int32_t kFeast = 100; ///< food from here up is meat, down from its negative bad meat
constexpr std::int32_t kRealGold = 10;          ///< more than this is treasure, else junk
constexpr std::uint32_t kShieldFlag = 0x200000; ///< the armour powerup that is a shield
constexpr std::string_view kKeySound = "S_PICKUPKEY";
constexpr std::string_view kMagicSound = "S_PICKUPMAGIC";
constexpr std::string_view kSpecialSound = "S_PICKUPSPECIAL";
constexpr std::string_view kShieldSound = "S_PICKUPSHIELD";
constexpr std::string_view kRuneSound = "S_PICKUPRUNE";

ItemTaking taken(std::int32_t count, std::string_view card, std::string_view sound) {
    ItemTaking taking;
    taking.outcome = ItemTaking::Outcome::Taken;
    taking.count = count;
    taking.card = card;
    taking.sound = sound;
    return taking;
}

ItemTaking refused(ItemTaking::Outcome outcome, std::int32_t left) {
    ItemTaking taking;
    taking.outcome = outcome;
    taking.left = left;
    return taking;
}

std::string_view foodCard(std::int32_t amount) {
    if (amount >= kFeast) {
        return "MEAT";
    }
    if (amount <= -kFeast) {
        return "BADMEAT";
    }
    return amount >= 0 ? "FRUIT" : "BADFRUIT";
}

} // namespace

std::int32_t mostHealth(std::int32_t level) {
    return std::min(kHealthPerLevel * (level - 1) + kBaseHealth, kHealthLimit);
}

ItemTaking takeItem(CharacterSave& save, const ItemOffer& offer, float powerupTime) {
    Inventory& inventory = save.progress().inventory;
    Relics& relics = save.progress().relics;
    if (offer.kind < 0 || offer.kind > static_cast<std::int32_t>(ItemKind::GargoyleKey)) {
        return {};
    }
    switch (static_cast<ItemKind>(offer.kind)) {
    case ItemKind::Gold:
        save.gold = std::min(save.gold + offer.amount, kMostGold);
        return taken(offer.amount, offer.amount > kRealGold ? "GOLD" : "JUNK", kMagicSound);
    case ItemKind::Keys: {
        const std::int32_t got = inventory.addKeys(offer.amount);
        if (got == 0) {
            return refused(ItemTaking::Outcome::KeysFull, offer.amount);
        }
        ItemTaking taking = taken(got, got > 1 ? "KEY_RING" : "KEY", kKeySound);
        if (got < offer.amount) {
            taking.outcome = ItemTaking::Outcome::PartTaken;
            taking.left = offer.amount - got;
        }
        return taking;
    }
    case ItemKind::Potion: {
        // Every potion of the item goes, or as many as fit: what does not fit is lost.
        const std::int32_t got =
            inventory.addPotions(static_cast<std::int32_t>(offer.flags), offer.amount);
        if (got == 0) {
            return refused(ItemTaking::Outcome::PotionsFull, offer.amount);
        }
        return taken(0, "MAGIC", kMagicSound);
    }
    case ItemKind::Food: {
        const std::int32_t most = mostHealth(experienceLevel(save.experience()));
        const std::int32_t health = save.health();
        if (offer.amount >= 0 && health >= most) {
            return refused(ItemTaking::Outcome::HealthFull, offer.amount);
        }
        save.progress().health = std::clamp(health + offer.amount, 1, most);
        ItemTaking taking = taken(offer.amount, foodCard(offer.amount), {});
        taking.ate = offer.amount >= 0;
        taking.hurt = offer.amount < 0;
        return taking;
    }
    case ItemKind::WeaponPowerup:
    case ItemKind::ArmorPowerup:
    case ItemKind::SpeedPowerup:
    case ItemKind::MagicPowerup:
    case ItemKind::SpecialPowerup:
        inventory.addPowerup(offer.kind, offer.flags, static_cast<float>(offer.amount),
                             offer.strength * powerupTime);
        return taken(0, "SPECIALS",
                     offer.kind == static_cast<std::int32_t>(ItemKind::ArmorPowerup) &&
                             (offer.flags & kShieldFlag) != 0
                         ? kShieldSound
                         : kSpecialSound);
    case ItemKind::Runestone:
        if (!relics.addRune(offer.amount)) {
            return refused(ItemTaking::Outcome::AlreadyHeld, offer.amount);
        }
        return taken(offer.amount, "RUNESTONE", kRuneSound);
    case ItemKind::Legend:
        if (!relics.addLegend(offer.amount)) {
            return {};
        }
        return taken(offer.amount, "LEGEND", kMagicSound);
    case ItemKind::GargoyleKey: {
        const std::int32_t pieces = relics.addGargoylePiece(offer.amount);
        if (pieces < 0) {
            return {};
        }
        return taken(pieces, "GOLDNICON", kMagicSound);
    }
    case ItemKind::Scroll: {
        // Read where it lies: the page it names goes up, the scroll goes, nothing is kept.
        ItemTaking taking;
        taking.outcome = ItemTaking::Outcome::Shown;
        taking.count = offer.amount - 1;
        return taking;
    }
    default: return {};
    }
}

} // namespace gdl::game
