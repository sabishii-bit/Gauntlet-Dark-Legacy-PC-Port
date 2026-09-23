#include "engine/ui/TextPainter.h"

#include <cstdint>

#include "engine/core/Assert.h"

namespace gdl {

void TextPainter::setFont(const BitmapFont* font, const Texture* texture) {
    m_font = font;
    m_texture = texture;
}

int TextPainter::measure(std::string_view text, float scale) const {
    int width = 0;
    for (const char c : text) {
        width += advance(static_cast<std::uint8_t>(c), scale);
    }
    return width;
}

int TextPainter::lineHeight(float scale) const {
    return m_font == nullptr ? 0 : static_cast<int>(static_cast<float>(m_font->height()) * scale);
}

int TextPainter::leftEdge(int x, std::string_view text, float scale) const {
    return x < 0 ? -x - measure(text, scale) / 2 : x;
}

int TextPainter::draw(Canvas& canvas, int x, int y, std::string_view text,
                      const TextStyle& style) const {
    GDL_VERIFY(ready(), "TextPainter::draw without a font");
    const Texture* texture = style.texture != nullptr ? style.texture : m_texture;
    const auto sheetWidth = static_cast<float>(m_texture->width());
    const auto sheetHeight = static_cast<float>(m_texture->height());
    const auto cellHeight = static_cast<float>(m_font->height());
    const auto expand = static_cast<float>(style.expand);

    int penX = leftEdge(x, text, style.scale);
    for (const char c : text) {
        const auto code = static_cast<std::uint8_t>(c);
        const BitmapGlyph* glyph = m_font->glyph(code);
        if (glyph != nullptr && glyph->width > 0) {
            const auto cellWidth = static_cast<float>(glyph->width);
            const float inset = kCellInset * style.scale;
            const Rect area{static_cast<float>(penX) + inset - expand,
                            static_cast<float>(y) + inset - expand,
                            (cellWidth - 2.0f * kCellInset) * style.scale + 2.0f * expand,
                            (cellHeight - 2.0f * kCellInset) * style.scale + 2.0f * expand};
            const Rect uv{(static_cast<float>(glyph->x) + kCellInset) / sheetWidth,
                          (static_cast<float>(glyph->y) + kCellInset) / sheetHeight,
                          (cellWidth - 2.0f * kCellInset) / sheetWidth,
                          (cellHeight - 2.0f * kCellInset) / sheetHeight};
            canvas.draw(*texture, area, uv, style.color);
        }
        penX += advance(code, style.scale);
    }
    return penX;
}

int TextPainter::advance(std::uint8_t code, float scale) const {
    if (m_font == nullptr) {
        return 0;
    }
    const BitmapGlyph* glyph = m_font->glyph(code);
    if (glyph != nullptr && glyph->width > 0) {
        return static_cast<int>(static_cast<float>(glyph->width) * scale);
    }
    if (code == ' ') {
        return static_cast<int>(static_cast<float>(m_font->spaceWidth()) * scale);
    }
    return 0;
}

} // namespace gdl
