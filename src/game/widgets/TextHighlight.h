#pragma once

#include <string_view>

#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

/**
 * The one way text is marked out across the menus and screens: the glyphs keep their ink
 * (white, or a value's own colour) and a purple glow is laid under them from the glow sheet,
 * pulsing while a row waits for a choice and steady while the pointer rests on it. The ink is
 * never the glow's colour, so a word under the pointer never turns purple itself.
 */
class TextHighlight {
public:
    static constexpr Color kGlow = Color::rgba(130, 0, 234);
    static constexpr s32 kPulseTicks = 40;
    static constexpr s32 kPulseHoldTicks = 5;

    /** The glow's strength at `ticks`: full under the pointer, pulsing for a marked row,
     * none for the rest. */
    static u8 glowAlpha(bool marked, bool hovered, s32 ticks);
    /** Draws `text` at (x, y) in `ink`, over the glow when `alpha` is above zero. Without a
     * glow sheet the glyph sheet stands in for it, as the menus do. */
    static void draw(Canvas& canvas, const TextPainter& painter, s32 x, s32 y,
                     std::string_view text, f32 scale, const Texture* glowSheet, u8 alpha,
                     Color ink = Color::white(), s32 expand = 0);
};

} // namespace gdl::game
