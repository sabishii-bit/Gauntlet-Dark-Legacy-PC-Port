#pragma once

#include "engine/ui/Canvas.h"

namespace gdl::game {
/** Native trigger-camera bars, extended across wide displays by the canvas. */
inline void drawCinematicBars(Canvas& canvas, f32 height) {
    constexpr f32 kTop = 48.0f / 384.0f;
    constexpr f32 kBottom = 80.0f / 384.0f;
    canvas.fillHorizontalBand(0, height * kTop, Color::black());
    canvas.fillHorizontalBand(height * (1 - kBottom), height * kBottom, Color::black());
}
} // namespace gdl::game
