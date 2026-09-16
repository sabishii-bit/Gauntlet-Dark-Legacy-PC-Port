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

void Image::setPixel(u32 x, u32 y, Color color) {
    const auto p = row(y).subspan(usize{x} * 4, 4);
    p[0] = color.r;
    p[1] = color.g;
    p[2] = color.b;
    p[3] = color.a;
}

} // namespace gdl
