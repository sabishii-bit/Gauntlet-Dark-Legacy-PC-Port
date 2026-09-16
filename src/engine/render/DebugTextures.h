#pragma once

#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** RGBA8 pixels of a size×size checkerboard with cell×cell squares; (0,0) gets `even`. */
std::vector<u8> makeCheckerboardRgba8(u32 size, u32 cell, Color even, Color odd);

} // namespace gdl
