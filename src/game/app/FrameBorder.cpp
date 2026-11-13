#include "game/app/FrameBorder.h"

#include "engine/core/Types.h"

namespace gdl::game {

void drawFrameBorder(Canvas& canvas, RenderDevice& device, Vec2 frame, Extent2D display) {
    canvas.begin(device, makeLetterboxProjection(frame.x, frame.y, static_cast<f32>(display.width),
                                                 static_cast<f32>(display.height)));
    canvas.maskAboveAndBelow(0, frame.y);
    canvas.end();
}

} // namespace gdl::game
