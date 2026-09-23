#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/math/Math.h"

namespace gdl {

/** CPU-side RGBA8 image, rows stored top to bottom. */
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    static Image filled(std::uint32_t width, std::uint32_t height, Color color);

    std::size_t rowBytes() const { return std::size_t{width} * 4; }
    std::span<std::uint8_t> row(std::uint32_t y) {
        return std::span(pixels).subspan(y * rowBytes(), rowBytes());
    }
    std::span<const std::uint8_t> row(std::uint32_t y) const {
        return std::span(pixels).subspan(y * rowBytes(), rowBytes());
    }

    Color pixel(std::uint32_t x, std::uint32_t y) const;
    void setPixel(std::uint32_t x, std::uint32_t y, Color color);

    /** Gives every fully transparent texel the average colour of its nearest opaque
     * neighbours (alpha untouched), spreading outward until none is left beside one, so
     * that filtering across a cut-out edge blends into the texture's own colour rather than
     * whatever the file hid behind alpha zero. */
    void bleedIntoTransparent();
};

} // namespace gdl
