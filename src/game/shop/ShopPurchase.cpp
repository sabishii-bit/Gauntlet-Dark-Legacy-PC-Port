#include "game/shop/ShopPurchase.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {
// Shop type is not an inventory subtype. These are the buy driver's effects,
// including charged weapons (-1 lifetime), not field-pickup amounts.
constexpr std::array<PowerupSlot, 41> kPowerups{
    {{},
     {},
     {90, powerup::kWeapon, 0, damage::kReflect},
     {},
     {30, powerup::kSpecial, 0, powerup::kGrowth},
     {},
     {},
     {},
     {},
     {30, powerup::kArmor, 0, armor::kReflect},
     {45, powerup::kSpecial, 0, powerup::kPhoenix},
     {30, powerup::kWeapon, 0, damage::kRapidFire},
     {45, powerup::kWeapon, 0, damage::kThreeWayShot},
     {-1, powerup::kWeapon, 3, damage::kHammer},
     {15, powerup::kArmor, 0, armor::kElectricArmor},
     {15, powerup::kArmor, 0, armor::kFireArmor},
     {20, powerup::kArmor, 0, armor::kInvulnerable | armor::kGoldInvulnerable},
     {},
     {60, powerup::kSpecial, 0, powerup::kLevitation},
     {30, powerup::kSpecial, 0, powerup::kGrowth},
     {90, powerup::kWeapon, 0, damage::kFire},
     {90, powerup::kWeapon, 0, damage::kElectric},
     {90, powerup::kWeapon, 0, damage::kLight},
     {90, powerup::kWeapon, 0, damage::kAcid},
     {-1, powerup::kWeapon, 5, damage::kSuperShot},
     {-1, powerup::kSpecial, 5, powerup::kFireBreath},
     {-1, powerup::kSpecial, 5, powerup::kLightningBreath},
     {-1, powerup::kSpecial, 5, powerup::kAcidBreath},
     {40, powerup::kSpecial, 4, powerup::kSpeedBoost},
     {15, powerup::kSpecial, 0, powerup::kEnemyShrink},
     {15, powerup::kSpecial, 0, powerup::kInvisible},
     {30, powerup::kArmor, 0, armor::kInvulnerable},
     {120, powerup::kSpecial, 0, powerup::kXRay},
     {15, powerup::kArmor, 0, armor::kResistAcid | armor::kImmuneGas},
     {45, powerup::kWeapon, 0, damage::kFiveWayShot},
     {120, powerup::kArmor, 0, armor::kAntiDeath},
     {120, powerup::kSpecial, 0, powerup::kHandOfDeath},
     {120, powerup::kSpecial, 0, powerup::kHealthVamp},
     {120, powerup::kSpecial, 0, powerup::kMikey},
     {25, powerup::kSpecial, 0, powerup::kStopTime},
     {60, powerup::kSpecial, 0, powerup::kPojo}}};
bool valid(const ShopItem& item) {
    return item.type >= 0 && item.type < static_cast<s32>(kPowerups.size()) && item.price >= 0 &&
           item.price <= 99999 && item.amount >= 0 && item.amount <= 99999;
}
bool matches(const PowerupSlot& slot, const ShopItem& item) {
    const auto& effect = kPowerups[static_cast<usize>(item.type)];
    // Elemental amulets use an enum, not a bit mask (light is 3).
    return effect.held() && slot.held() && slot.kind == effect.kind && slot.flags == effect.flags;
}
} // namespace
bool expandShopCatalog(ShopCatalog& catalog, const StringTable& strings) {
    constexpr s32 kPrice = 1000;
    // Four offers retain their existing buy-driver effects; Pojo uses its field
    // pickup's 60-second duration and the ordinary class powerup-time scale.
    const std::array<ShopItem, 5> offers{{
        {"SHP_SUPER_INVULN", std::string(strings.get("shop.goldInvulnerability")), 0.825f, 16,
         kPrice, 1},
        {"SHP_POJOEGG", std::string(strings.get("shop.pojo")), 0.95f, 40, kPrice, 1},
        {"SHP_5WAY", std::string(strings.get("shop.fiveWayShot")), 0.95f, 34, kPrice, 1},
        {"SHP_SHRINK", std::string(strings.get("shop.shrink")), 0.95f, 29, kPrice, 1},
        {"SHP_HOURGLASS", std::string(strings.get("shop.timestop")), 0.95f, 39, kPrice, 1},
    }};
    return catalog.insertBefore(36, offers);
}
ShopResult shopEligibility(const CharacterSave& save, const ClassStats& stats,
                           const ShopItem& item) {
    if (!valid(item)) {
        return ShopResult::Invalid;
    }
    if (item.type == ShopItem::kExit) {
        return ShopResult::Exit;
    }
    if (save.gold < item.price) {
        return ShopResult::InsufficientGold;
    }
    const auto& inventory = save.progress().inventory;
    if ((item.type == ShopItem::kKey && inventory.keys >= Inventory::kMostKeys) ||
        (item.type == ShopItem::kPotion && inventory.potions.size() >= Inventory::kMostPotions) ||
        (item.type == ShopItem::kHealthRefill &&
         save.health() >= mostHealth(experienceLevel(save.experience())))) {
        return ShopResult::Full;
    }
    if (item.type >= 5 && item.type <= 8) {
        if (save.character == kSumnerClass) {
            return ShopResult::Full;
        }
        const StatBlock values =
            displayStats(stats, experienceLevel(save.experience()), save.progress());
        if (values.values[static_cast<usize>(item.type - 5)] >= kMaxStat) {
            return ShopResult::Full;
        }
    }
    return ShopResult::Bought;
}
ShopResult buyShopItem(CharacterSave& save, const ClassStats& stats, const ShopItem& item,
                       s32 potionKind) {
    const auto result = shopEligibility(save, stats, item);
    if (result != ShopResult::Bought) {
        return result;
    }
    if (item.type == ShopItem::kPotion && (potionKind < 1 || potionKind > 4)) {
        return ShopResult::Invalid;
    }
    auto& progress = save.progress();
    switch (item.type) {
    case 1: progress.inventory.addKeys(1); break;
    case 3: progress.inventory.addPotions(potionKind, 1); break;
    case 5: progress.fightAdd += 10; break;
    case 6: progress.speedAdd += 10; break;
    case 7: progress.armorAdd += 10; break;
    case 8: progress.magicAdd += 10; break;
    case 17:
        progress.health =
            std::min(save.health() + item.amount, mostHealth(experienceLevel(save.experience())));
        break;
    default: {
        const auto& effect = kPowerups[static_cast<usize>(item.type)];
        progress.inventory.addPowerup(effect.kind, effect.flags, effect.charge,
                                      effect.strength * stats.powerupTime, false);
        break;
    }
    }
    save.gold -= item.price;
    return ShopResult::Bought;
}
bool ownsShopItem(const CharacterSave& save, const ShopItem& item) {
    if (!valid(item)) {
        return false;
    }
    const auto& inventory = save.progress().inventory;
    if (item.type == ShopItem::kKey) {
        return inventory.keys > 0;
    }
    if (item.type == ShopItem::kPotion) {
        return !inventory.potions.empty();
    }
    return std::ranges::any_of(inventory.powerups,
                               [&](const auto& slot) { return matches(slot, item); });
}
ShopResult sellShopItem(CharacterSave& save, const ShopItem& item) {
    if (!ownsShopItem(save, item)) {
        return ShopResult::NotOwned;
    }
    auto& inventory = save.progress().inventory;
    if (item.type == ShopItem::kKey) {
        inventory.spendKey();
    } else if (item.type == ShopItem::kPotion) {
        inventory.takePotion();
    } else {
        auto& slot = *std::ranges::find_if(inventory.powerups,
                                           [&](const auto& held) { return matches(held, item); });
        slot = {};
    }
    save.gold = std::min(99999, save.gold + item.price * 3 / 4);
    return ShopResult::Sold;
}
} // namespace gdl::game
