#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "game/hud/BossMeter.h"

namespace gdl::game {

/** Draws boss meter `meter`'s strips in a Legends look, each `strip` across from the meter's left
 * at `top`, both parts under `shade`: the strip's back whole, then its fill cropped, not
 * squeezed, to the piece's width of `widths`. A piece of no width shows its back alone. */
void drawLegendsBossMeter(Canvas& canvas, RenderDevice& device, const BossMeter& meter, f32 strip,
                          f32 top, std::span<const s32> widths, Color shade);

} // namespace gdl::game
