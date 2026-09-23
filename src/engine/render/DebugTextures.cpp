#include "engine/render/DebugTextures.h"

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

std::vector<u8> makeCheckerboardRgba8(u32 size, u32 cell, Color even, Color odd) {
    GDL_VERIFY(size > 0 && cell > 0, "checkerboard size and cell must be non-zero");
    std::vector<u8> pixels;
    pixels.reserve(static_cast<usize>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
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
