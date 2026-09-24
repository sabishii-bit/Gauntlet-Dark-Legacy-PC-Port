#pragma once

#include <span>
#include <vector>

#include "engine/assets/ShopCatalog.h"
#include "engine/core/Types.h"

namespace gdl::game {
/** Scroll coordinates are icon tops, not uniformly sized menu rows. */
struct ShopLayout {
    static constexpr s32 kTop = 72;
    static constexpr s32 kBottom = 224;
    static constexpr s32 kFadeRange = 64;
    std::vector<s32> rows;
    f32 target = 0;

    static ShopLayout make(std::span<const ShopItem> items, usize cursor, s32 fontHeight);
    static u8 opacity(f32 y);
};
} // namespace gdl::game
