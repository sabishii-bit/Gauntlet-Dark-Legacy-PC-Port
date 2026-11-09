#pragma once

#include <string_view>

#include "engine/ui/TextPainter.h"

namespace gdl::game {

/** Native entry-title presentation, with its clock supplied by the scene's tick state. */
class LegendsDreamcastEntrance {
public:
    static constexpr s32 kDurationTicks = 91;
    static f32 scale(s32 elapsedTicks);
    /** `title` is LEVL's authored display name; `font` is Dreamcast font handle 6. */
    static void draw(Canvas& canvas, const TextPainter& font, std::string_view title, f32 width,
                     s32 elapsedTicks);
};

} // namespace gdl::game
