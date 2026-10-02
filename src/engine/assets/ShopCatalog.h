#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
/** A runtime view of an entry in the game's item catalog. */
struct ShopItem {
    std::string texture;
    std::string description;
    f32 scale = 1.0f;
    s32 type = 0;
    s32 price = 0;
    s32 amount = 0;
};
class ShopCatalog {
public:
    bool load(const std::filesystem::path& file);
    /** Resolves SHPDATA/SHOP.WAD; exported catalog remains a development fixture. */
    bool loadRoot(const std::filesystem::path& root);
    static ShopCatalog fromJson(std::string_view text);
    const std::vector<ShopItem>& items() const { return m_items; }

private:
    void validate() const;
    std::vector<ShopItem> m_items;
};
} // namespace gdl
