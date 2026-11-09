#include "expansions/dreamcast/menu/LegendsDreamcastDialogue.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {

bool LegendsDreamcastDialogue::load(RenderDevice& device, const std::filesystem::path& nativeRoot) {
    m_text.setFont(nullptr, nullptr);
    m_menuText.setFont(nullptr, nullptr);
    m_menuTextures = {};
    m_fontSheet.reset();
    m_menuFontSheet.reset();
    m_scroll.reset();
    m_arrows.reset();
    const AssetLocator files{nativeRoot};
    const auto smallFont = files.find("fonts/8hi_fonts5.fnt");
    const auto menuFont = files.find("fonts/font32.fnt");
    const auto objects = files.find("static/objects.rom");
    const auto pixels = files.find("static/textures.rom");
    if (!smallFont || !menuFont || !objects || !pixels) {
        return false;
    }
    try {
        // 1ST_READ.BIN 8c148624/8c14864c: font handles 1/6 and their space widths.
        BitmapFont small;
        BitmapFont large;
        if (!small.load(*smallFont, 8) || !large.load(*menuFont, 16)) {
            return false;
        }
        const auto textures =
            formats::decodeLegendsDreamcastTextures(readFile(*objects), readFile(*pixels));
        const auto upload = [&](std::string_view name) -> std::unique_ptr<Texture> {
            const auto found = std::ranges::find(textures, name, &formats::LegendsMapTexture::name);
            if (found == textures.end() || found->image.pixels.empty()) {
                return {};
            }
            return device.createTexture(
                {found->width, found->height, TextureFilter::Linear, TextureWrap::ClampToEdge},
                found->image.pixels);
        };
        auto fontSheet = upload("8HI_FONTS5");
        auto menuSheet = upload("FONT32");
        auto scroll = upload("TEXTSCROLL");
        auto arrows = upload("ARROWS");
        if (!fontSheet || !menuSheet || !scroll || !arrows) {
            return false;
        }
        m_font = std::move(small);
        m_menuFont = std::move(large);
        m_fontSheet = std::move(fontSheet);
        m_menuFontSheet = std::move(menuSheet);
        m_scroll = std::move(scroll);
        m_arrows = std::move(arrows);
        m_text.setFont(&m_font, m_fontSheet.get());
        m_menuText.setFont(&m_menuFont, m_menuFontSheet.get());
        // No atlas override: topics use font 1, while entrance titles use font 6.
        // Dreamcast has no GC parchment/glow/garamond sheets with matching glyph UVs.
        m_menuTextures.backdrop = m_scroll.get();
        m_menuTextures.arrows = m_arrows.get();
        return true;
    } catch (const std::exception& error) {
        log::warn("Dreamcast dialogue: {}", error.what());
        return false;
    }
}

std::optional<MenuDefinition>
LegendsDreamcastDialogue::nativeTopics(std::string_view /*title*/,
                                       std::span<const MenuItem> topics) const {
    if (!m_text.ready() || !m_scroll || topics.empty()) {
        return {};
    }
    MenuDefinition result;
    // 8c065e60 sizes TEXTSCROLL from the four advice labels, not the screen.
    // There is no header. The fifth, lowercase "exit" sits below them at right.
    result.scale = kScale;
    result.items.assign(topics.begin(), topics.end());
    result.rowGap = static_cast<s32>(16 * kScale) - m_text.lineHeight(kScale);
    result.showCursor = false;
    result.colors.off = Color::rgba(0, 31, 0);
    result.colors.on = Color::rgba(127, 0, 0);
    result.colors.hi = Color::rgba(0, 0, 0, 0);
    result.backdrop = "TEXTSCROLL";
    s32 width = 0;
    for (const auto& topic : topics) {
        width = std::max(width, m_text.measure(topic.text, kScale));
    }
    const s32 side = width / 4;
    const s32 row = static_cast<s32>(16 * kScale);
    const s32 gap = static_cast<s32>(8 * kScale);
    const s32 content = static_cast<s32>(topics.size()) * row + gap;
    const s32 margin = static_cast<s32>(static_cast<f32>(content) * .1875f);
    result.backdropWidth = std::min(512, width + 2 * side);
    result.backdropHeight = std::min(320, content + 2 * margin);
    result.backdropX = 256 - result.backdropWidth / 2;
    result.backdropY = 160 - result.backdropHeight / 2;
    const s32 top = result.backdropY + margin + m_font.height() / 2;
    for (usize i = 0; i < topics.size(); ++i) {
        const bool exit = i + 1 == topics.size();
        const s32 x = exit ? result.backdropX + result.backdropWidth - side -
                                 m_text.measure(topics[i].text, kScale)
                           : -256;
        result.itemPositions.emplace_back(x, top + static_cast<s32>(i) * row + (exit ? gap : 0));
    }
    return result;
}

DialoguePage LegendsDreamcastDialogue::layout(std::string_view page, f32 scale,
                                              std::string_view prompt) const {
    DialoguePage result;
    result.lines = ScrollBox::splitLines(page);
    s32 width = m_text.measure(prompt, scale);
    for (const auto& line : result.lines) {
        width = std::max(width, m_text.measure(line, scale));
    }
    // 8c065e60/8c062d60: TEXTSCROLL uses quarter-width side margins and 3/16-height
    // vertical margins. Ten lines leave room for the port's device-independent prompt.
    const f32 boxWidth = std::min(512.0f, static_cast<f32>(width) * 1.5f);
    const f32 textHeight = static_cast<f32>(result.lines.size()) * 16 * scale;
    const f32 gap = 8 * scale;
    const f32 contentHeight = textHeight + (prompt.empty() ? 0.0f : gap + 16 * scale);
    const f32 margin = contentHeight * .1875f;
    const f32 boxHeight = std::min(320.0f, contentHeight + 2 * margin);
    result.area = {256 - boxWidth * .5f, 160 - boxHeight * .5f, boxWidth, boxHeight};
    result.textTop = static_cast<s32>(result.area.y + margin);
    result.promptY = result.textTop + static_cast<s32>(textHeight + gap);
    return result;
}

void LegendsDreamcastDialogue::draw(Canvas& canvas, const DialoguePage& page, f32 scale,
                                    std::string_view prompt, u8 promptAlpha) const {
    if (!m_scroll || !m_text.ready()) {
        return;
    }
    canvas.draw(*m_scroll, page.area, Color::white());
    TextStyle ink{.scale = scale, .color = Color::rgba(0, 31, 0)};
    s32 y = page.textTop;
    for (const auto& line : page.lines) {
        m_text.draw(canvas, -256, y, line, ink);
        y += static_cast<s32>(16 * scale);
    }
    ink.color = Color::rgba(127, 0, 0, static_cast<u8>(128 + promptAlpha / 2));
    m_text.draw(canvas, -256, page.promptY, prompt, ink);
}

} // namespace gdl::game
