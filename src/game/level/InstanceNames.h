#pragma once
#include <string>
#include <string_view>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

namespace gdl::game {

/** The layout's record of an item instance, or null for one with none. */
const ItemInfo* instanceRecord(const WorldLayout& layout, const ItemInstance& instance);
/** The name an item instance goes by: its own, else its record's; empty for no instance. */
std::string instanceName(const WorldLayout& layout, s32 instance);
/** What a pickup record's subtype is: "keys", "gold", "powerup"... */
std::string_view pickupKindName(s32 subtype);

} // namespace gdl::game
