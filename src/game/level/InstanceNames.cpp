#include "game/level/InstanceNames.h"

#include "game/players/ItemPickup.h"

namespace gdl::game {

const ItemInfo* instanceRecord(const WorldLayout& layout, const ItemInstance& instance) {
    const auto& infos = layout.itemInfos();
    return instance.info >= 0 && static_cast<usize>(instance.info) < infos.size()
               ? &infos[static_cast<usize>(instance.info)]
               : nullptr;
}

std::string instanceName(const WorldLayout& layout, s32 instance) {
    const auto& instances = layout.itemInstances();
    if (instance < 0 || static_cast<usize>(instance) >= instances.size()) {
        return {};
    }
    const ItemInstance& placed = instances[static_cast<usize>(instance)];
    if (!placed.name.empty()) {
        return placed.name;
    }
    const ItemInfo* record = instanceRecord(layout, placed);
    return record != nullptr ? record->name : std::string{};
}

std::string_view pickupKindName(s32 subtype) {
    switch (static_cast<ItemKind>(subtype)) {
    case ItemKind::Gold: return "gold";
    case ItemKind::Keys: return "keys";
    case ItemKind::Food: return "food";
    case ItemKind::Potion: return "potion";
    case ItemKind::WeaponPowerup: return "weapon powerup";
    case ItemKind::ArmorPowerup: return "armor powerup";
    case ItemKind::SpeedPowerup: return "speed powerup";
    case ItemKind::MagicPowerup: return "magic powerup";
    case ItemKind::SpecialPowerup: return "special powerup";
    case ItemKind::Runestone: return "runestone";
    case ItemKind::Legend: return "legend item";
    case ItemKind::Scroll: return "scroll";
    case ItemKind::Crystal: return "crystal";
    case ItemKind::GargoyleKey: return "gargoyle piece";
    }
    return "pickup";
}

} // namespace gdl::game
