#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gdl::formats {

struct FontGlyph {
    std::int32_t code = 0;  ///< character code
    std::int32_t width = 0; ///< advance and cell width in texels
    std::int32_t x = 0;     ///< cell position in the font texture
    std::int32_t y = 0;
};

/** A font file from the FONTS directory: the glyph height plus one cell per character, all cut from
 * one texture. */
struct FontFile {
    std::int32_t height = 0;
    std::vector<FontGlyph> glyphs;

    /** Parses the little-endian file; throws FormatError. */
    static FontFile parse(std::span<const std::uint8_t> file);
};

} // namespace gdl::formats
