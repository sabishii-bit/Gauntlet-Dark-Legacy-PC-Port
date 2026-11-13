#include "game/players/ItemPickup.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/Types.h"

#include "game/characters/Progression.h"
#include "game/characters/Vitality.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {

constexpr s32 kMostGold = 99999;
constexpr s32 kFeast = 100;   ///< food from here up is meat, down from its negative bad meat
constexpr s32 kRealGold = 10; ///< more than this is treasure, else junk
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
    static constexpr std::array kCues{Cue{powerup::kWeapon, damage::kThreeWayShot, 37},
                                      Cue{powerup::kWeapon, damage::kFiveWayShot, 47},
                                      Cue{powerup::kWeapon, damage::kReflect, 38},
                                      Cue{powerup::kWeapon, damage::kSuperShot, 48},
                                      Cue{powerup::kWeapon, damage::kHammer, 86},
                                      Cue{powerup::kWeapon, damage::kRapidFire, 87},
                                      Cue{powerup::kArmor, armor::kGoldInvulnerable, 54},
                                      Cue{powerup::kArmor, armor::kInvulnerable, 35},
                                      Cue{powerup::kArmor, armor::kAntiDeath, 49},
                                      Cue{powerup::kArmor, armor::kReflect, 52},
                                      Cue{powerup::kArmor, armor::kFireArmor, 91},
                                      Cue{powerup::kArmor, armor::kElectricArmor, 92},
                                      Cue{powerup::kArmor, armor::kImmuneGas, 132},
                                      Cue{powerup::kSpecial, powerup::kInvisible, 36},
                                      Cue{powerup::kSpecial, powerup::kXRay, 39},
                                      Cue{powerup::kSpecial, powerup::kStopTime, 51},
                                      Cue{powerup::kSpecial, powerup::kLevitation, 53},
                                      Cue{powerup::kSpecial, powerup::kFireBreath, 81},
                                      Cue{powerup::kSpecial, powerup::kAcidBreath, 82},
                                      Cue{powerup::kSpecial, powerup::kLightningBreath, 83},
                                      Cue{powerup::kSpecial, powerup::kPhoenix, 84},
                                      Cue{powerup::kSpecial, powerup::kGrowth, 88},
                                      Cue{powerup::kSpecial, powerup::kEnemyShrink, 89},
                                      Cue{powerup::kSpecial, powerup::kPojo, 93},
                                      Cue{powerup::kSpecial, powerup::kSkorneMask, 98},
                                      Cue{powerup::kSpecial, powerup::kSkorneHorns, 99},
                                      Cue{powerup::kSpecial, powerup::kLeftGauntlet, 100},
                                      Cue{powerup::kSpecial, powerup::kRightGauntlet, 100},
                                      Cue{powerup::kSpecial, powerup::kTurbo, 113},
                                      Cue{powerup::kSpecial, powerup::kMikey, 148},
                                      Cue{powerup::kSpecial, powerup::kHandOfDeath, 149},
                                      Cue{powerup::kSpecial, powerup::kHealthVamp, 150}};
    for (const Cue& cue : kCues) {
        if (kind == cue.kind && (flags & cue.flag) != 0) {
            return cue.message;
        }
    }
    const u32 element = flags & damage::kElementMask;
    if (kind == powerup::kWeapon && element >= damage::kFire && element <= damage::kAcid) {
        return 39 + static_cast<s32>(element);
    }
    if (kind == powerup::kSpeed) {
        return 32;
    }
    if (kind == powerup::kMagic) {
        return 33;
    }
    return -1;
}

/** sounds_evt.c::fn_8009CEE0: these effects also play when the help was seen already. */
std::string_view powerupSound(s32 kind, u32 flags) {
    if (kind == powerup::kArmor && (flags & armor::kFireArmor) != 0) {
        return kShieldSound;
    }
    if (kind == powerup::kSpecial) {
        if ((flags & powerup::kLevitation) != 0) {
            return "S_LEVITATEUP";
        }
        if ((flags & powerup::kGrowth) != 0) {
            return "S_GROW";
        }
        if ((flags & powerup::kEnemyShrink) != 0) {
            return "S_SHRINK";
        }
        if ((flags & powerup::kPojo) != 0) {
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

s32 goldCapacity(const CharacterSave& save) {
    return kMostGold - std::clamp(save.gold, 0, kMostGold);
}

ItemTaking takeItem(CharacterSave& save, const ItemOffer& offer, f32 powerupTime,
                    bool activatePowerups) {
    Inventory& inventory = save.progress().inventory;
    Relics& relics = save.progress().relics;
    if (offer.kind < 0 || offer.kind > static_cast<s32>(ItemKind::GargoyleKey)) {
        return {};
    }
    switch (static_cast<ItemKind>(offer.kind)) {
    case ItemKind::Gold:
        if (goldCapacity(save) == 0) {
            return refused(ItemTaking::Outcome::GoldFull, offer.amount);
        }
        save.gold = std::min(save.gold + offer.amount, kMostGold);
        // do_got_it records the offered gold independently of the capped wallet.
        save.progress().lifetime.goldFound += offer.amount;
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
        if (offer.amount >= 0 && healthFull(save)) {
            return refused(ItemTaking::Outcome::HealthFull, offer.amount);
        }
        if (offer.amount < 0) {
            // do_got_it sends poisoned food through damage_player with the gas flag.
            // In particular, Gold Invulnerability converts it to ten-percent healing;
            // unlike ordinary food healing, that may exceed the level's food cap.
            const auto worn = PowerupEffects::of(inventory);
            const auto damage =
                Damage::modify(-static_cast<f32>(offer.amount), damage::kGas, worn.armor, 0, false);
            save.progress().health = std::clamp(
                static_cast<s32>(std::lround(static_cast<f32>(save.health()) - damage.amount)), 1,
                kHealthLimit);
        } else {
            heal(save, offer.amount);
        }
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
                             offer.strength * powerupTime, activatePowerups);
        // Inventory retains native manual activation for Turbo and Mikey. Auto-Use Item
        // extends pickup activation to Turbo without consuming it on load or in the shop.
        if (activatePowerups && offer.kind == powerup::kSpecial &&
            (offer.flags & powerup::kTurbo) != 0) {
            for (auto& slot : inventory.powerups) {
                if (slot.kind == offer.kind && slot.flags == offer.flags && slot.held()) {
                    slot.on = true;
                }
            }
        }
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
