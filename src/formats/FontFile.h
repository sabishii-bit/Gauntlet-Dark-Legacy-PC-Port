#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

struct FontGlyph {
    s32 code = 0;  ///< character code
    s32 width = 0; ///< advance and cell width in texels
    s32 x = 0;     ///< cell position in the font texture
    s32 y = 0;
};

/** A font file from the FONTS directory: the glyph height plus one cell per character, all cut from
 * one texture. */
struct FontFile {
    s32 height = 0;
    std::vector<FontGlyph> glyphs;

    /** Parses the little-endian file; throws FormatError. */
    static FontFile parse(std::span<const u8> file);
};

} // namespace gdl::formats
