#include "game/players/ShopPurchase.h"

#include <algorithm>
#include <array>

#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"

namespace gdl::game {
namespace {
// Shop type is not an inventory subtype. These are the buy driver's effects,
// including charged weapons (-1 lifetime), not field-pickup amounts.
constexpr std::array<PowerupSlot, 40> kPowerups{{{},
                                                 {},
                                                 {90, 5, 0, 0x200000},
                                                 {},
                                                 {30, 9, 0, 0x100},
                                                 {},
                                                 {},
                                                 {},
                                                 {},
                                                 {30, 6, 0, 0x20000},
                                                 {45, 9, 0, 0x80},
                                                 {30, 5, 0, 0x20000000},
                                                 {45, 5, 0, 0x80000},
                                                 {-1, 5, 3, 0x10000000},
                                                 {15, 6, 0, 0x400000},
                                                 {15, 6, 0, 0x200000},
                                                 {20, 6, 0, 0x110000},
                                                 {},
                                                 {60, 9, 0, 1},
                                                 {30, 9, 0, 0x100},
                                                 {90, 5, 0, 1},
                                                 {90, 5, 0, 2},
                                                 {90, 5, 0, 3},
                                                 {90, 5, 0, 4},
                                                 {-1, 5, 5, 0x100000},
                                                 {-1, 9, 5, 0x10},
                                                 {-1, 9, 5, 0x40},
                                                 {-1, 9, 5, 0x20},
                                                 {40, 9, 4, 0x10000},
                                                 {15, 9, 0, 0x200},
                                                 {15, 9, 0, 4},
                                                 {30, 6, 0, 0x10000},
                                                 {120, 9, 0, 2},
                                                 {15, 6, 0, 0x2008},
                                                 {45, 5, 0, 0x400000},
                                                 {120, 6, 0, 0x80000},
                                                 {120, 9, 0, 0x200000},
                                                 {120, 9, 0, 0x400000},
                                                 {120, 9, 0, 0x100000},
                                                 {25, 9, 0, 8}}};
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
ShopResult shopEligibility(const CharacterSave& save, const ClassStats& stats,
                           const ShopItem& item) {
    if (!valid(item)) {
        return ShopResult::Invalid;
    }
    if (item.type == 0) {
        return ShopResult::Exit;
    }
    if (save.gold < item.price) {
        return ShopResult::InsufficientGold;
    }
    const auto& inventory = save.progress().inventory;
    if ((item.type == 1 && inventory.keys >= Inventory::kMostKeys) ||
        (item.type == 3 && inventory.potions.size() >= Inventory::kMostPotions) ||
        (item.type == 17 && save.health() >= mostHealth(experienceLevel(save.experience())))) {
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
    if (item.type == 3 && (potionKind < 1 || potionKind > 4)) {
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
                                      effect.strength * stats.powerupTime);
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
    if (item.type == 1) {
        return inventory.keys > 0;
    }
    if (item.type == 3) {
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
    if (item.type == 1) {
        inventory.spendKey();
    } else if (item.type == 3) {
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
