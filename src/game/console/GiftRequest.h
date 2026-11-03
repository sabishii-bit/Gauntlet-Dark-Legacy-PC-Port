#pragma once
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"

#include "game/characters/Inventory.h"
#include "game/console/ConsoleCommand.h"
#include "game/players/ItemPickup.h"

namespace gdl::game {

/** What `get` hands over, or tells. */
struct GiftRequest {
    enum class Kind : u8 { Item, Experience, Level, Info };
    static constexpr s32 kDefaultKeys = 1;
    static constexpr s32 kDefaultPotions = 1;
    static constexpr s32 kDefaultPotionKind = 1; ///< fire
    static constexpr s32 kDefaultGold = 100;
    static constexpr s32 kDefaultHealth = 100;
    static constexpr s32 kDefaultCharges = 10;    ///< a charged powerup's, unless given
    static constexpr f32 kDefaultSeconds = 60.0f; ///< a timed powerup's, unless given
    /** What get's usage says under its forms. */
    static constexpr std::string_view kUsageNote = "list items names the items.";

    Kind kind = Kind::Item;
    ItemOffer offer;  ///< the item, as if it lay in the level
    s32 amount = 0;   ///< the experience to add or the level to reach
    std::string name; ///< the item's, or the thing `info` is asked of (empty: what is selected)
};

/** A powerup `get` hands over by name, and what `list items` says it does. */
struct GiftPowerup {
    std::string_view name;
    std::string_view summary;
    s32 kind;
    u32 flags;
    bool charged; ///< spent a charge at a time rather than timed
    f32 charge;   ///< a stat boost, for one that has one
};

/** get's forms, each with what it does, in the order its usage lists them. */
std::vector<std::pair<std::string, std::string>> giftUsageRows();
std::string giftUsage();
/** Reads `get`'s arguments; the usage or the reason for an unknown item or amount when they
 * make no sense. */
gdl::Expected<GiftRequest, std::string> parseGift(ConsoleCommand::Arguments arguments);
/** What `get` says of what came of `gift`. */
std::string giftReport(const GiftRequest& gift, const ItemTaking& taking);
/** Every powerup `get` knows, in the order `list items` names them. */
std::span<const GiftPowerup> giftPowerups();
/** The console's name for a held powerup, or its kind and flags for one it has none for. */
std::string powerupName(const PowerupSlot& slot);
/** The console's name for a potion's kind. */
std::string_view potionName(s32 kind);

} // namespace gdl::game
