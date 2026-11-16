#include "expansions/n64/hud/LegendsN64Scroll.h"

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {

void drawLegendsN64Scroll(Canvas& canvas, const Texture& scroll, const Rect& area) {
    constexpr f32 kInset = 0.5f; ///< texels in from a piece's edges
    const auto width = static_cast<f32>(scroll.width());
    const auto height = static_cast<f32>(scroll.height());
    if (width <= 0 || height <= 0) {
        return;
    }
    const auto piece = [&](f32 top, f32 tall, s32 row, s32 rows) {
        canvas.draw(scroll, Rect{area.x, top, area.width, tall},
                    Rect{kInset / width, (static_cast<f32>(row) + kInset) / height,
                         (width - 2 * kInset) / width, static_cast<f32>(rows - 1) / height},
                    Color::white());
    };
    constexpr auto kCap = static_cast<f32>(LegendsN64Scroll::kCapRows);
    piece(area.y, kCap, LegendsN64Scroll::kTopCapRow, LegendsN64Scroll::kCapRows);
    if (const f32 middle = area.height - 2 * kCap; middle > 0) {
        piece(area.y + kCap, middle, LegendsN64Scroll::kMiddleRow, LegendsN64Scroll::kMiddleRows);
    }
    piece(area.bottom() - kCap, kCap, LegendsN64Scroll::kBottomCapRow, LegendsN64Scroll::kCapRows);
}

} // namespace gdl::game
