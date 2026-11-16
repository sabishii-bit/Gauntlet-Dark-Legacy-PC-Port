#pragma once

#include <span>

#include "engine/assets/BitmapFont.h"
#include "engine/render/Image.h"

namespace gdl::formats {

/** Native packed five-byte glyph directory and its I4, IA4 or IA8 atlas. */
struct LegendsN64Font {
    BitmapFont font;
    Image atlas; ///< with clear rows added down to the lowest glyph's bottom
    static LegendsN64Font parse(std::span<const u8> bytes);
};

} // namespace gdl::formats
