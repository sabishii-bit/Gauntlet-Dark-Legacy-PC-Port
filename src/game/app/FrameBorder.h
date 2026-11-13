#pragma once

#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {

/** Blacks out a `display` above and below the `frame` fitted into it, whatever a screen drew
 * there, as the GameCube's video border shows its 640x448 frame within the 480-line picture
 * (GXAdjustForOverscan by 16 lines at 0x800CBA80); beside the frame a wide display goes on
 * showing the view. */
void drawFrameBorder(Canvas& canvas, RenderDevice& device, Vec2 frame, Extent2D display);

} // namespace gdl::game
