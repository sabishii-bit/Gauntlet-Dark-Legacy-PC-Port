#pragma once
#include "engine/assets/ShopCatalog.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"

namespace gdl::game {
enum class ShopResult : u8 { Bought, Sold, Exit, InsufficientGold, Full, NotOwned, Invalid };
/** Adds the five optional carried powerups between Anti-Death and Hand of Death. */
bool expandShopCatalog(ShopCatalog& catalog, const StringTable& strings);
/** Eligibility is side-effect free. A refused transaction changes neither money nor inventory. */
ShopResult shopEligibility(const CharacterSave& save, const ClassStats& stats,
                           const ShopItem& item);
ShopResult buyShopItem(CharacterSave& save, const ClassStats& stats, const ShopItem& item,
                       s32 potionKind);
bool ownsShopItem(const CharacterSave& save, const ShopItem& item);
ShopResult sellShopItem(CharacterSave& save, const ShopItem& item);
} // namespace gdl::game
