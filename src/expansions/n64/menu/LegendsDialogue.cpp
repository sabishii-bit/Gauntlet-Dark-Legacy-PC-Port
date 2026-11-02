#include "expansions/n64/menu/LegendsDialogue.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {

bool LegendsDialogue::load(RenderDevice& device, const std::filesystem::path& nativeRoot) {
    m_text.setFont(nullptr, nullptr);
    m_fontSheet.reset();
    m_scroll.reset();
    const AssetLocator files{nativeRoot};
    const auto font = files.find("Font/glfm3.fnt");
    const auto objects = files.find("hud/objects.wad");
    if (!font || !objects) {
        return false;
    }
    try {
        auto decoded = formats::LegendsN64Font::parse(readFile(*font));
        auto textures = formats::decodeLegendsN64Textures(readFile(*objects));
        const auto scroll = std::ranges::find(textures, std::string{"TEXTSCROLL"},
                                              &formats::LegendsN64Texture::name);
        if (scroll == textures.end() || scroll->image.pixels.empty() || scroll->width != 64 ||
            scroll->height != 32) {
            return false;
        }
        m_font = std::move(decoded.font);
        m_fontSheet = device.createTexture({decoded.atlas.width, decoded.atlas.height,
                                            TextureFilter::Linear, TextureWrap::ClampToEdge},
                                           decoded.atlas.pixels);
        m_scroll = device.createTexture(
            {scroll->width, scroll->height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            scroll->image.pixels);
        m_text.setFont(&m_font, m_fontSheet.get());
        return true;
    } catch (const std::exception& error) {
        log::warn("N64 dialogue: {}", error.what());
        return false;
    }
}

DialoguePage LegendsDialogue::layout(std::string_view page, f32 scale,
                                     std::string_view prompt) const {
    DialoguePage result;
    result.lines = ScrollBox::splitLines(page);
    const s32 lineHeight = m_text.lineHeight(scale);
    const auto count = static_cast<s32>(result.lines.size());
    // e00717a0/e0071a10: font5, 350-wide long-page scroll, origin40,40;
    // text starts six pixels below the cap, with native 12/70 horizontal inset.
    const f32 width = 350 * kScale;
    const f32 height = static_cast<f32>(count * lineHeight) + 16 * kScale +
                       (prompt.empty() ? 0.0f : static_cast<f32>(lineHeight));
    result.area = {40 * kScale, 40 * kScale, width, height};
    result.textTop = static_cast<s32>(result.area.y + 6 * kScale);
    result.promptY = result.textTop + count * lineHeight + static_cast<s32>(3 * kScale);
    return result;
}

void LegendsDialogue::draw(Canvas& canvas, const DialoguePage& page, f32 scale,
                           std::string_view prompt, u8 promptAlpha) const {
    if (!m_scroll || !m_text.ready()) {
        return;
    }
    // Authored caps 0..7 and23..30 remain eight native pixels tall. Only
    // rows8..22 stretch to the text, rather than stretching a whole GC sheet.
    const f32 cap = 8 * kScale;
    const auto strip = [&](f32 y, f32 height, f32 v, f32 rows) {
        canvas.draw(*m_scroll, {page.area.x, y, page.area.width, height},
                    {.5f / 64, (v + .5f) / 32, 63.0f / 64, (rows - 1) / 32}, Color::white());
    };
    strip(page.area.y, cap, 0, 8);
    strip(page.area.y + cap, page.area.height - cap * 2, 8, 15);
    strip(page.area.bottom() - cap, cap, 23, 8);
    TextStyle ink{.scale = scale, .color = Color::black()};
    const auto left = static_cast<s32>(page.area.x + 60 * kScale);
    s32 y = page.textTop;
    for (const auto& line : page.lines) {
        m_text.draw(canvas, left, y, line, ink);
        y += m_text.lineHeight(scale);
    }
    // Retain the port's device-independent prompt without GC glyphs or glow art.
    ink.color.a = static_cast<u8>(128 + promptAlpha / 2);
    m_text.draw(canvas, -static_cast<s32>(page.area.x + page.area.width * .5f), page.promptY,
                prompt, ink);
}

} // namespace gdl::game
