#include "engine/render/Image.h"

#include <cstddef>
#include <cstdint>

namespace gdl {

Image Image::filled(std::uint32_t width, std::uint32_t height, Color color) {
    Image image{width, height, {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            image.setPixel(x, y, color);
        }
    }
    return image;
}

Color Image::pixel(std::uint32_t x, std::uint32_t y) const {
    const auto p = row(y).subspan(std::size_t{x} * 4, 4);
    return Color::rgba(p[0], p[1], p[2], p[3]);
}

void Image::bleedIntoTransparent() {
    if (width == 0 || height == 0) {
        return;
    }
    // Texels that carry colour: opaque ones, then those coloured by an earlier pass.
    std::vector<std::uint8_t> coloured(std::size_t{width} * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            coloured[std::size_t{y} * width + x] = pixel(x, y).a != 0 ? 1 : 0;
        }
    }
    std::vector<std::uint8_t> next;
    bool spread = true;
    while (spread) {
        spread = false;
        next = coloured;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (coloured[std::size_t{y} * width + x] != 0) {
                    continue;
                }
                std::uint32_t r = 0;
                std::uint32_t g = 0;
                std::uint32_t b = 0;
                std::uint32_t count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto nx = static_cast<std::int64_t>(x) + dx;
                        const auto ny = static_cast<std::int64_t>(y) + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width ||
                            ny >= height ||
                            coloured[static_cast<std::size_t>(ny) * width +
                                     static_cast<std::size_t>(nx)] == 0) {
                            continue;
                        }
                        const Color near =
                            pixel(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
                        r += near.r;
                        g += near.g;
                        b += near.b;
                        ++count;
                    }
                }
                if (count == 0) {
                    continue;
                }
                setPixel(x, y,
                         Color::rgba(static_cast<std::uint8_t>(r / count),
                                     static_cast<std::uint8_t>(g / count),
                                     static_cast<std::uint8_t>(b / count), 0));
                next[std::size_t{y} * width + x] = 1;
                spread = true;
            }
        }
        coloured.swap(next);
    }
}

void Image::setPixel(std::uint32_t x, std::uint32_t y, Color color) {
    const auto p = row(y).subspan(std::size_t{x} * 4, 4);
    p[0] = color.r;
    p[1] = color.g;
    p[2] = color.b;
    p[3] = color.a;
}

} // namespace gdl
