#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** One character cell: its code and where it sits in the font texture; height is the font's. */
struct BitmapGlyph {
    s32 code = 0;
    s32 width = 0;
    s32 x = 0;
    s32 y = 0;
};

/** The cell table of one bitmap font; characters without a cell take up no space except ' '. */
class BitmapFont {
public:
    static BitmapFont fromGlyphs(s32 height, s32 spaceWidth, std::vector<BitmapGlyph> glyphs);

    /** Reads an unpacked font manifest; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file, s32 spaceWidth);

    bool loaded() const { return m_height > 0; }
    s32 height() const { return m_height; }
    s32 spaceWidth() const { return m_spaceWidth; }
    usize glyphCount() const { return m_glyphs.size(); }

    /** The cell for a character code, or nullptr when the font has none. */
    const BitmapGlyph* glyph(u8 code) const;

private:
    void index();

    s32 m_height = 0;
    s32 m_spaceWidth = 0;
    std::vector<BitmapGlyph> m_glyphs;
    std::array<s16, 256> m_lookup{};
};

} // namespace gdl
