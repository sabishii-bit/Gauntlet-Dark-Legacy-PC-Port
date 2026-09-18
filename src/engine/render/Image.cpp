#include "engine/render/Image.h"

namespace gdl {

Image Image::filled(u32 width, u32 height, Color color) {
    Image image{width, height, {}};
    image.pixels.resize(usize{width} * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            image.setPixel(x, y, color);
        }
    }
    return image;
}

Color Image::pixel(u32 x, u32 y) const {
    const auto p = row(y).subspan(usize{x} * 4, 4);
    return Color::rgba(p[0], p[1], p[2], p[3]);
}

void Image::bleedIntoTransparent() {
    if (width == 0 || height == 0) {
        return;
    }
    // Texels that carry colour: opaque ones, then those coloured by an earlier pass.
    std::vector<u8> coloured(usize{width} * height, 0);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            coloured[usize{y} * width + x] = pixel(x, y).a != 0 ? 1 : 0;
        }
    }
    std::vector<u8> next;
    bool spread = true;
    while (spread) {
        spread = false;
        next = coloured;
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                if (coloured[usize{y} * width + x] != 0) {
                    continue;
                }
                u32 r = 0;
                u32 g = 0;
                u32 b = 0;
                u32 count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto nx = static_cast<s64>(x) + dx;
                        const auto ny = static_cast<s64>(y) + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width ||
                            ny >= height ||
                            coloured[static_cast<usize>(ny) * width + static_cast<usize>(nx)] ==
                                0) {
                            continue;
                        }
                        const Color near = pixel(static_cast<u32>(nx), static_cast<u32>(ny));
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
                         Color::rgba(static_cast<u8>(r / count), static_cast<u8>(g / count),
                                     static_cast<u8>(b / count), 0));
                next[usize{y} * width + x] = 1;
                spread = true;
            }
        }
        coloured.swap(next);
    }
}

void Image::setPixel(u32 x, u32 y, Color color) {
    const auto p = row(y).subspan(usize{x} * 4, 4);
    p[0] = color.r;
    p[1] = color.g;
    p[2] = color.b;
    p[3] = color.a;
}

} // namespace gdl
