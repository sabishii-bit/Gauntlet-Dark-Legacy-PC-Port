#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** The colour of an N64 IA4 texel, `bits` its four: three of intensity, its grey, widened to
 * eight as the RDP widens them, by repeating them, over one of alpha. */
Color legendsN64IntensityAlpha4(u32 bits);

/** The colour of an N64 IA8 texel, `bits` its byte: a nibble of intensity, its grey, over a
 * nibble of alpha. */
Color legendsN64IntensityAlpha8(u32 bits);

} // namespace gdl::formats
