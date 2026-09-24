#include "formats/ShopWad.h"

#include <cmath>

#include "engine/core/Error.h"

#include "formats/WadDirectory.h"

namespace gdl::formats {
std::vector<ShopItemRecord> parseShopWad(std::span<const u8> bytes) {
    constexpr usize kStride = 80;
    const auto directory = readWadDirectory(bytes, "shop wad");
    const auto* items = findWadSection(directory, "ITEM");
    if (items == nullptr || items->offset > bytes.size() ||
        items->count > (bytes.size() - items->offset) / kStride) {
        throw FormatError("shop wad: missing or truncated ITEM section");
    }
    std::vector<ShopItemRecord> result;
    for (u32 i = 0; i < items->count; ++i) {
        const usize offset = items->offset + i * kStride;
        ShopItemRecord item;
        item.texture = readWadText(bytes, offset, 32, "shop wad");
        item.description = readWadText(bytes, offset + 32, 32, "shop wad");
        item.scale = readWadF32(bytes, offset + 64, "shop wad");
        item.type = static_cast<s32>(readWadU32(bytes, offset + 68, "shop wad"));
        item.price = static_cast<s32>(readWadU32(bytes, offset + 72, "shop wad"));
        item.amount = static_cast<s32>(readWadU32(bytes, offset + 76, "shop wad"));
        if (!std::isfinite(item.scale) || item.scale <= 0 || item.price < 0 || item.type < 0 ||
            item.type > 39) {
            throw FormatError("shop wad: invalid item");
        }
        result.push_back(item);
    }
    return result;
}
} // namespace gdl::formats
