#include "engine/assets/ShopCatalog.h"

#include <cmath>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {
ShopCatalog ShopCatalog::fromJson(std::string_view text) {
    ShopCatalog catalog;
    try {
        const auto root = nlohmann::json::parse(text);
        const auto& items = root.at("items");
        if (!items.is_array() || items.empty() || items.size() > 64) {
            throw FormatError("shop: expected 1..64 items");
        }
        for (const auto& row : items) {
            ShopItem item{
                row.at("texture").get<std::string>(), row.at("description").get<std::string>(),
                row.at("scale").get<f32>(),           row.at("type").get<s32>(),
                row.at("price").get<s32>(),           row.at("amount").get<s32>()};
            if (!std::isfinite(item.scale) || item.scale <= 0 || item.scale > 16 || item.type < 0 ||
                item.type > 39 || item.price < 0 || item.price > 99999 || item.amount < 0 ||
                item.amount > 99999 ||
                (item.type == 0 && (!catalog.m_items.empty() || item.price != 0))) {
                throw FormatError("shop: invalid item");
            }
            catalog.m_items.push_back(std::move(item));
        }
        if (catalog.m_items.front().type != 0) {
            throw FormatError("shop: first item must be Exit");
        }
    } catch (const nlohmann::json::exception& e) {
        throw FormatError(std::string("shop: ") + e.what());
    }
    return catalog;
}
bool ShopCatalog::load(const std::filesystem::path& file) {
    m_items.clear();
    try {
        *this = fromJson(readTextFile(file));
        return true;
    } catch (const std::exception& e) {
        log::warn("Shop catalog {}: {}", file.string(), e.what());
        return false;
    }
}
} // namespace gdl
