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
    int expand = 0;                   ///< pixels added on every side of each glyph
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

    int measure(std::string_view text, float scale = 1.0f) const;
    int lineHeight(float scale = 1.0f) const;

    /** Resolves a possibly-negative x to the line's left edge. */
    int leftEdge(int x, std::string_view text, float scale) const;

    /** Draws one line and returns the x just past its last glyph. */
    int draw(Canvas& canvas, int x, int y, std::string_view text, const TextStyle& style) const;

private:
    int advance(std::uint8_t code, float scale) const;

    const BitmapFont* m_font = nullptr;
    const Texture* m_texture = nullptr;
};

} // namespace gdl
