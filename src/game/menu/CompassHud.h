#pragma once

#include <array>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {
/** World-axis compass: north is +Z, east is +X; rotates with the view, not player heading. */
class CompassHud {
public:
    static std::array<Vec2, 4> directions(f32 cameraYaw);
    static void draw(Canvas& canvas, const TextPainter& text, const StringTable* strings, f32 width,
                     f32 cameraYaw);
};
} // namespace gdl::game
