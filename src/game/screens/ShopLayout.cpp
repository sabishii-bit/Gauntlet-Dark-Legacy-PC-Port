#include "game/screens/ShopLayout.h"

#include <algorithm>

namespace gdl::game {
ShopLayout ShopLayout::make(std::span<const ShopItem> items, usize cursor, s32 fontHeight) {
    ShopLayout layout;
    s32 y = 0;
    for (const auto& item : items) {
        layout.rows.push_back(y);
        if (!item.texture.empty()) {
            y += 24;
        }
        if (!item.description.empty()) {
            const auto lines = 1 + static_cast<s32>(std::ranges::count(item.description, '\n'));
            y += lines * static_cast<s32>(static_cast<f32>(fontHeight) * item.scale * 0.5f) + 16;
        }
    }
    if (cursor >= layout.rows.size()) {
        return layout;
    }
    const s32 selected = layout.rows[cursor];
    s32 base = (kTop + kBottom) / 2;
    if (base - selected > kTop) {
        base = kTop + selected;
    }
    if (base + layout.rows.back() - selected < kBottom) {
        base = kBottom - layout.rows.back() + selected;
    }
    layout.target = static_cast<f32>(base - selected);
    return layout;
}
u8 ShopLayout::opacity(f32 y) {
    const f32 distance = std::max({static_cast<f32>(kTop) - y, y - kBottom, 0.0f});
    return static_cast<u8>(255 - std::clamp(distance * 510 / kFadeRange, 0.0f, 255.0f));
}
} // namespace gdl::game
