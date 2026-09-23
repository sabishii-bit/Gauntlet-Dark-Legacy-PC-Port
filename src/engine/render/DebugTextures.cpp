#include "engine/render/DebugTextures.h"

#include <cstddef>
#include <cstdint>

#include "engine/core/Assert.h"

namespace gdl {

std::vector<std::uint8_t> makeCheckerboardRgba8(std::uint32_t size, std::uint32_t cell, Color even,
                                                Color odd) {
    GDL_VERIFY(size > 0 && cell > 0, "checkerboard size and cell must be non-zero");
    std::vector<std::uint8_t> pixels;
    pixels.reserve(static_cast<std::size_t>(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool isOdd = (((x / cell) + (y / cell)) & 1U) != 0;
            const Color c = isOdd ? odd : even;
            pixels.push_back(c.r);
            pixels.push_back(c.g);
            pixels.push_back(c.b);
            pixels.push_back(c.a);
        }
    }
    return pixels;
}

} // namespace gdl
