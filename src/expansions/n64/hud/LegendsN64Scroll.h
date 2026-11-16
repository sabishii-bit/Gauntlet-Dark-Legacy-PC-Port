#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {

/** The rows of an N64 TEXTSCROLL picture that FUN_e00717a0 draws as three pieces: a cap above
 * and below, drawn whole, and the rows between them, stretched to the scroll's height. */
struct LegendsN64Scroll {
    static constexpr s32 kCapRows = 8;
    static constexpr s32 kTopCapRow = 0; ///< rows 0..7
    static constexpr s32 kMiddleRow = 8; ///< rows 8..22
    static constexpr s32 kMiddleRows = 15;
    static constexpr s32 kBottomCapRow = 23; ///< rows 23..30
};

/** Draws `scroll` over `area` of the N64's frame, in frame units: its caps kCapRows tall and
 * the rows between them stretched over the rest. Each piece is sampled from half a texel inside
 * its rows and the picture's sides, so filtering never pulls in the next piece's rows. */
void drawLegendsN64Scroll(Canvas& canvas, const Texture& scroll, const Rect& area);

} // namespace gdl::game
