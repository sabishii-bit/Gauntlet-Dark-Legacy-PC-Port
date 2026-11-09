#include "expansions/dreamcast/menu/LegendsDreamcastEntrance.h"

#include <algorithm>

#include "game/tower/LevelArrivalPresentation.h"

namespace gdl::game {

f32 LegendsDreamcastEntrance::scale(s32 elapsedTicks) {
    // 1ST_READ.BIN 8c078760 starts at .025 for 91 ticks. Function 8c078920
    // advances by .025 per native tick and caps at 2, drawing LEVL+20 with
    // font 6 in white at (256, 48-trunc(16*scale)): the shared title geometry.
    // Clock callback 8c083580 converts seconds to 61440 fixed-point units,
    // then shifts by 10: these are 60 Hz ticks, not 30 Hz simulation steps.
    if (elapsedTicks < 0 || elapsedTicks >= kDurationTicks) {
        return 0;
    }
    return std::min(2.0f, .025f + .025f * static_cast<f32>(elapsedTicks));
}

void LegendsDreamcastEntrance::draw(Canvas& canvas, const TextPainter& font, std::string_view title,
                                    f32 width, s32 elapsedTicks) {
    LevelArrivalPresentation::drawTitleAt(canvas, font, title, width, scale(elapsedTicks));
}

} // namespace gdl::game
