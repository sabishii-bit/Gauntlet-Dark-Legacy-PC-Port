#include "expansions/shared/hud/LegendsBossMeters.h"

#include <algorithm>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "game/hud/BossMeter.h"

namespace gdl::game {

void drawLegendsBossMeter(Canvas& canvas, RenderDevice& device, const BossMeter& meter, f32 strip,
                          f32 top, std::span<const s32> widths, Color shade) {
    const BossMeter::Layout layout = meter.layout();
    const s32 pieces = std::min(layout.pieces, BossMeter::kMostPieces);
    for (s32 piece = 0; piece < pieces; ++piece) {
        const f32 x = static_cast<f32>(layout.left) + (strip * static_cast<f32>(piece));
        if (const Texture* back =
                meter.pieceTexture(device, BossMeter::MeterPart::Background, piece)) {
            canvas.draw(*back, Rect{x, top, strip, static_cast<f32>(back->height())}, shade);
        }
        const auto at = static_cast<usize>(piece);
        const f32 width =
            at < widths.size() ? std::clamp(static_cast<f32>(widths[at]), 0.0f, strip) : 0.0f;
        if (width <= 0) {
            continue;
        }
        if (const Texture* fill = meter.pieceTexture(device, BossMeter::MeterPart::Fill, piece)) {
            // Cropped, not squeezed: the fill's left part.
            canvas.draw(*fill, Rect{x, top, width, static_cast<f32>(fill->height())},
                        Rect{0, 0, width / strip, 1}, shade);
        }
    }
}

} // namespace gdl::game
