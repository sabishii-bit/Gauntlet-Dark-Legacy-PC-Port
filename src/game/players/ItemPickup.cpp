#include "game/players/ItemPickup.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr s32 kMostGold = 99999;
constexpr s32 kBaseHealth = 500;
constexpr s32 kHealthPerLevel = 100;
constexpr s32 kHealthLimit = 9999;
constexpr s32 kFeast = 100;   ///< food from here up is meat, down from its negative bad meat
constexpr s32 kRealGold = 10; ///< more than this is treasure, else junk
constexpr u32 kShieldFlag = 0x200000; ///< the armour powerup that is a shield
constexpr std::string_view kKeySound = "S_PICKUPKEY";
constexpr std::string_view kMagicSound = "S_PICKUPMAGIC";
constexpr std::string_view kSpecialSound = "S_PICKUPSPECIAL";
constexpr std::string_view kShieldSound = "S_PICKUPSHIELD";
constexpr std::string_view kRuneSound = "S_PICKUPRUNE";

/** items.c's ordered pickup dispatch. A multi-flag item announces only its first match;
 * amulets compare the low nibble as an element, not as independent bits. */
s32 powerupMessage(s32 kind, u32 flags) {
    struct Cue {
        s32 kind;
        u32 flag;
        s32 message;
    };
    static constexpr std::array kCues{
        Cue{5, 0x80000, 37},   Cue{5, 0x400000, 47},   Cue{5, 0x200000, 38},
        Cue{5, 0x100000, 48},  Cue{5, 0x10000000, 86}, Cue{5, 0x20000000, 87},
        Cue{6, 0x100000, 54},  Cue{6, 0x10000, 35},    Cue{6, 0x80000, 49},
        Cue{6, 0x20000, 52},   Cue{6, 0x200000, 91},   Cue{6, 0x400000, 92},
        Cue{6, 0x2000, 132},   Cue{9, 4, 36},          Cue{9, 2, 39},
        Cue{9, 8, 51},         Cue{9, 1, 53},          Cue{9, 0x10, 81},
        Cue{9, 0x20, 82},      Cue{9, 0x40, 83},       Cue{9, 0x80, 84},
        Cue{9, 0x100, 88},     Cue{9, 0x200, 89},      Cue{9, 0x400, 93},
        Cue{9, 0x2000, 98},    Cue{9, 0x1000, 99},     Cue{9, 0x8000, 100},
        Cue{9, 0x4000, 100},   Cue{9, 0x80000, 113},   Cue{9, 0x100000, 148},
        Cue{9, 0x200000, 149}, Cue{9, 0x400000, 150}};
    for (const Cue& cue : kCues) {
        if (kind == cue.kind && (flags & cue.flag) != 0) {
            return cue.message;
        }
    }
    if (kind == 5 && (flags & 15) >= 1 && (flags & 15) <= 4) {
        return 39 + static_cast<s32>(flags & 15);
    }
    if (kind == 7) {
        return 32;
    }
    if (kind == 8) {
        return 33;
    }
    return -1;
}

/** sounds_evt.c::fn_8009CEE0: these effects also play when the help was seen already. */
std::string_view powerupSound(s32 kind, u32 flags) {
    if (kind == 6 && (flags & kShieldFlag) != 0) {
        return kShieldSound;
    }
    if (kind == 9) {
        if ((flags & 1) != 0) {
            return "S_LEVITATEUP";
        }
        if ((flags & 0x100) != 0) {
            return "S_GROW";
        }
        if ((flags & 0x200) != 0) {
            return "S_SHRINK";
        }
        if ((flags & 0x400) != 0) {
            return "S_POJO";
        }
    }
    return kSpecialSound;
}

ItemTaking taken(s32 count, std::string_view card, std::string_view sound) {
    ItemTaking taking;
    taking.outcome = ItemTaking::Outcome::Taken;
    taking.count = count;
    taking.card = card;
    taking.sound = sound;
    return taking;
}

ItemTaking refused(ItemTaking::Outcome outcome, s32 left) {
    ItemTaking taking;
    taking.outcome = outcome;
    taking.left = left;
    return taking;
}

std::string_view foodCard(s32 amount) {
    if (amount >= kFeast) {
        return "MEAT";
    }
    if (amount <= -kFeast) {
        return "BADMEAT";
    }
    return amount >= 0 ? "FRUIT" : "BADFRUIT";
}

} // namespace

s32 mostHealth(s32 level) {
    return std::min(kHealthPerLevel * (level - 1) + kBaseHealth, kHealthLimit);
}

ItemTaking takeItem(CharacterSave& save, const ItemOffer& offer, f32 powerupTime) {
    Inventory& inventory = save.progress().inventory;
    Relics& relics = save.progress().relics;
    if (offer.kind < 0 || offer.kind > static_cast<s32>(ItemKind::GargoyleKey)) {
        return {};
    }
    switch (static_cast<ItemKind>(offer.kind)) {
    case ItemKind::Gold:
        save.gold = std::min(save.gold + offer.amount, kMostGold);
        return taken(offer.amount, offer.amount > kRealGold ? "GOLD" : "JUNK", kMagicSound);
    case ItemKind::Keys: {
        const s32 got = inventory.addKeys(offer.amount);
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
        const s32 got = inventory.addPotions(static_cast<s32>(offer.flags), offer.amount);
        if (got == 0) {
            return refused(ItemTaking::Outcome::PotionsFull, offer.amount);
        }
        return taken(0, "MAGIC", kMagicSound);
    }
    case ItemKind::Food: {
        const s32 most = mostHealth(experienceLevel(save.experience()));
        const s32 health = save.health();
        if (offer.amount >= 0 && health >= most) {
            return refused(ItemTaking::Outcome::HealthFull, offer.amount);
        }
        save.progress().health = std::clamp(health + offer.amount, 1, most);
        ItemTaking taking = taken(offer.amount, foodCard(offer.amount), {});
        taking.ate = offer.amount >= 0;
        taking.hurt = offer.amount < 0;
        if (offer.amount >= 100) {
            taking.message = 15;
        } else if (offer.amount >= 50) {
            taking.message = 16;
        } else if (offer.amount < 0) {
            taking.message = 28;
        }
        return taking;
    }
    case ItemKind::WeaponPowerup:
    case ItemKind::ArmorPowerup:
    case ItemKind::SpeedPowerup:
    case ItemKind::MagicPowerup:
    case ItemKind::SpecialPowerup: {
        inventory.addPowerup(offer.kind, offer.flags, static_cast<f32>(offer.amount),
                             offer.strength * powerupTime);
        ItemTaking taking = taken(0, "SPECIALS", powerupSound(offer.kind, offer.flags));
        taking.message = powerupMessage(offer.kind, offer.flags);
        return taking;
    }
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
        const s32 pieces = relics.addGargoylePiece(offer.amount);
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
