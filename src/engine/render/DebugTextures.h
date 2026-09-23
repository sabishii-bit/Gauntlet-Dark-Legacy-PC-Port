#pragma once

#include <cstdint>
#include <vector>

#include "engine/math/Math.h"

namespace gdl {

/** RGBA8 pixels of a size×size checkerboard with cell×cell squares; (0,0) gets `even`. */
std::vector<std::uint8_t> makeCheckerboardRgba8(std::uint32_t size, std::uint32_t cell, Color even,
                                                Color odd);

} // namespace gdl
