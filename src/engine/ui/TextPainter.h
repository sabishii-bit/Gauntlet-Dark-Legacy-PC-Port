#pragma once

#include <cstdint>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl {

struct TextStyle {
    float scale = 1.0f;
    Color color = Color::white();
    const Texture* texture = nullptr; ///< glyph sheet to sample; the painter's own when null
    std::int32_t expand = 0;          ///< pixels added on every side of each glyph
};

/**
 * Draws bitmap-font lines in virtual pixels. A negative x centres the line on -x; y is the top
 * of the line. Characters the font lacks are skipped, except spaces which advance.
 */
class TextPainter {
public:
    /** How far inside its cell a glyph is sampled, in texels: half a texel keeps the
     * filtering from pulling in the cell's borders and its neighbours. */
    static constexpr float kCellInset = 0.5f;

    void setFont(const BitmapFont* font, const Texture* texture);

    const BitmapFont* font() const { return m_font; }
    bool ready() const { return m_font != nullptr && m_texture != nullptr; }

    std::int32_t measure(std::string_view text, float scale = 1.0f) const;
    std::int32_t lineHeight(float scale = 1.0f) const;

    /** Resolves a possibly-negative x to the line's left edge. */
    std::int32_t leftEdge(std::int32_t x, std::string_view text, float scale) const;

    /** Draws one line and returns the x just past its last glyph. */
    std::int32_t draw(Canvas& canvas, std::int32_t x, std::int32_t y, std::string_view text,
                      const TextStyle& style) const;

private:
    std::int32_t advance(std::uint8_t code, float scale) const;

    const BitmapFont* m_font = nullptr;
    const Texture* m_texture = nullptr;
};

} // namespace gdl
