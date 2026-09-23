#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace gdl {

/** One character cell: its code and where it sits in the font texture; height is the font's. */
struct BitmapGlyph {
    std::int32_t code = 0;
    std::int32_t width = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
};

/** The cell table of one bitmap font; characters without a cell take up no space except ' '. */
class BitmapFont {
public:
    static BitmapFont fromGlyphs(std::int32_t height, std::int32_t spaceWidth,
                                 std::vector<BitmapGlyph> glyphs);

    /** Reads an unpacked font manifest; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file, std::int32_t spaceWidth);

    bool loaded() const { return m_height > 0; }
    std::int32_t height() const { return m_height; }
    std::int32_t spaceWidth() const { return m_spaceWidth; }
    std::size_t glyphCount() const { return m_glyphs.size(); }

    /** The cell for a character code, or nullptr when the font has none. */
    const BitmapGlyph* glyph(std::uint8_t code) const;

private:
    void index();

    std::int32_t m_height = 0;
    std::int32_t m_spaceWidth = 0;
    std::vector<BitmapGlyph> m_glyphs;
    std::array<std::int16_t, 256> m_lookup{};
};

} // namespace gdl
