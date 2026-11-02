#include "game/widgets/TextHighlight.h"

#include "game/widgets/OptionMenu.h"

namespace gdl::game {

u8 TextHighlight::glowAlpha(bool marked, bool hovered, s32 ticks) {
    if (hovered) {
        return 255;
    }
    return marked ? pulseOpacity(ticks, kPulseTicks, kPulseHoldTicks) : 0;
}

void TextHighlight::draw(Canvas& canvas, const TextPainter& painter, s32 x, s32 y,
                         std::string_view text, f32 scale, const Texture* glowSheet, u8 alpha,
                         Color ink, s32 expand) {
    if (alpha > 0) {
        TextStyle glow;
        glow.scale = scale;
        glow.color = kGlow.withAlpha(alpha);
        glow.texture = glowSheet;
        glow.expand = expand;
        painter.draw(canvas, x, y, text, glow);
    }
    painter.draw(canvas, x, y, text, TextStyle{scale, ink});
}

} // namespace gdl::game
