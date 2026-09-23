#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** CPU-side RGBA8 image, rows stored top to bottom. */
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;

    static Image filled(u32 width, u32 height, Color color);

    usize rowBytes() const { return usize{width} * 4; }
    std::span<u8> row(u32 y) { return std::span(pixels).subspan(y * rowBytes(), rowBytes()); }
    std::span<const u8> row(u32 y) const {
        return std::span(pixels).subspan(y * rowBytes(), rowBytes());
    }

    Color pixel(u32 x, u32 y) const;
    void setPixel(u32 x, u32 y, Color color);

    /** Gives every fully transparent texel the average colour of its nearest opaque
     * neighbours (alpha untouched), spreading outward until none is left beside one, so
     * that filtering across a cut-out edge blends into the texture's own colour rather than
     * whatever the file hid behind alpha zero. */
    void bleedIntoTransparent();
};

} // namespace gdl
