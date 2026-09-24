#pragma once
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {
struct ShopItemRecord {
    std::string texture;
    std::string description;
    f32 scale = 1;
    s32 type = 0;
    s32 price = 0;
    s32 amount = 0;
};
/** Reads SHPDATA/SHOP.WAD's 80-byte ITEM records. */
std::vector<ShopItemRecord> parseShopWad(std::span<const u8> bytes);
} // namespace gdl::formats
