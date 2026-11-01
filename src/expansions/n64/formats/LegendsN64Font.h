#pragma once

#include <span>

#include "engine/assets/BitmapFont.h"
#include "engine/render/Image.h"

namespace gdl::formats {

/** Native packed five-byte glyph directory and its embedded I4 coverage atlas. */
struct LegendsN64Font {
    BitmapFont font;
    Image atlas;
    static LegendsN64Font parse(std::span<const u8> bytes);
};

} // namespace gdl::formats
