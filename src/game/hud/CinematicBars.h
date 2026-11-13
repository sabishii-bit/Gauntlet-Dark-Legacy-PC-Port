#pragma once

#include "engine/ui/Canvas.h"

namespace gdl::game {
/** Native trigger-camera bars, out to the display's edges: across a wide one, and over the
 * margins above and below the frame on one narrower than it. */
inline void drawCinematicBars(Canvas& canvas, f32 height) {
    constexpr f32 kTop = 48.0f / 384.0f;
    constexpr f32 kBottom = 80.0f / 384.0f;
    canvas.maskAboveAndBelow(height * kTop, height * (1 - kBottom));
}
} // namespace gdl::game
