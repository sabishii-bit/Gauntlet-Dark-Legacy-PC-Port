#include "expansions/n64/menu/LegendsDialogue.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {

bool LegendsDialogue::load(RenderDevice& device, const std::filesystem::path& nativeRoot) {
    m_text.setFont(nullptr, nullptr);
    m_titleText.setFont(nullptr, nullptr);
    m_fontSheet.reset();
    m_titleFontSheet.reset();
    m_scroll.reset();
    m_menuTextures = {};
    m_ceremonyText.clear();
    const AssetLocator files{nativeRoot};
    const auto font = files.find("Font/glfm3.fnt");
    const auto titleFont = files.find("Font/bigfont.fnt");
    const auto objects = files.find("hud/objects.wad");
    if (!font || !titleFont || !objects) {
        return false;
    }
    try {
        auto decoded = formats::LegendsN64Font::parse(readFile(*font));
        auto decodedTitle = formats::LegendsN64Font::parse(readFile(*titleFont));
        auto textures = formats::decodeLegendsN64Textures(readFile(*objects));
        const auto scroll = std::ranges::find(textures, std::string{"TEXTSCROLL"},
                                              &formats::LegendsN64Texture::name);
        if (scroll == textures.end() || scroll->image.pixels.empty() || scroll->width != 64 ||
            scroll->height != 32) {
            return false;
        }
        m_font = std::move(decoded.font);
        m_titleFont = std::move(decodedTitle.font);
        m_fontSheet = device.createTexture({decoded.atlas.width, decoded.atlas.height,
                                            TextureFilter::Linear, TextureWrap::ClampToEdge},
                                           decoded.atlas.pixels);
        m_titleFontSheet =
            device.createTexture({decodedTitle.atlas.width, decodedTitle.atlas.height,
                                  TextureFilter::Linear, TextureWrap::ClampToEdge},
                                 decodedTitle.atlas.pixels);
        m_scroll = device.createTexture(
            {scroll->width, scroll->height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            scroll->image.pixels);
        m_text.setFont(&m_font, m_fontSheet.get());
        m_titleText.setFont(&m_titleFont, m_titleFontSheet.get());
        m_menuTextures.backdrop = m_scroll.get();
        if (const auto library = files.find("strlib.wad")) {
            const auto reference = formats::legendsN64StringReference(readFile(*library), 521);
            if (reference) {
                if (const auto bank = files.find("english/" + reference->bank + ".wad")) {
                    const auto strings = formats::decodeLegendsN64Text(readFile(*bank));
                    if (reference->index + 39 <= strings.size()) {
                        m_ceremonyText.assign(strings.begin() + reference->index,
                                              strings.begin() + reference->index + 39);
                    }
                }
            }
        }
        return true;
    } catch (const std::exception& error) {
        log::warn("N64 dialogue: {}", error.what());
        return false;
    }
}

void LegendsDialogue::drawCeremonyCaption(Canvas& canvas, u32 textId, f32 opacity,
                                          f32 width) const {
    if (textId < 521 || textId - 521 >= m_ceremonyText.size() || opacity <= 0 || !m_text.ready()) {
        return;
    }
    // Tower e0860620 selects font5, scale1, y30. e086098c centres each
    // caption at native x208 and fades over .3 seconds at both ends.
    m_text.draw(canvas, -static_cast<s32>(width / 2), static_cast<s32>(30 * kScale),
                m_ceremonyText[textId - 521],
                {.scale = kScale,
                 .color = Color::white().withAlpha(
                     static_cast<u8>(std::clamp(opacity, 0.0f, 1.0f) * 255))});
}

std::optional<MenuDefinition>
LegendsDialogue::nativeTopics(std::string_view title, std::span<const MenuItem> topics) const {
    if (!m_text.ready() || !m_titleText.ready() || !m_scroll || topics.empty()) {
        return {};
    }
    // e0046000 / e0041894: font8 title at 1.5, font5 rows at 1, a 15-pixel
    // title gap, five-pixel row gaps and TEXTSCROLL margins of 30 by 10.
    // Centre in the top three quarters, adapting the native gameplay viewport
    // to the port's 512x384 menu canvas rather than using the physical window.
    const s32 titleHeight = static_cast<s32>(m_titleFont.height() * 1.5f) + 15;
    const s32 rowHeight = m_font.height() + 5;
    const s32 contentHeight = titleHeight + static_cast<s32>(topics.size()) * rowHeight - 5;
    s32 width = m_titleText.measure(title, 1.5f);
    for (const auto& topic : topics) {
        width = std::max(width, m_text.measure(topic.text, 1));
    }
    const f32 top = 384 * .75f / 2 - static_cast<f32>(contentHeight / 2) * kScale;
    MenuDefinition result;
    result.items.assign(topics.begin(), topics.end());
    result.scale = kScale;
    result.showCursor = false;
    result.colors = {Color::black(), Color::rgba(0, 132, 0), Color::rgba(0, 0, 0, 0)};
    result.backdrop = "TEXTSCROLL";
    result.backdropX = static_cast<s32>(256 - (width / 2 + 30) * kScale);
    result.backdropY = static_cast<s32>(top - 10 * kScale);
    result.backdropWidth = static_cast<s32>((width / 2 + 30) * 2 * kScale);
    result.backdropHeight = static_cast<s32>((contentHeight + 20) * kScale);
    result.itemPositions.reserve(topics.size());
    for (usize i = 0; i < topics.size(); ++i) {
        result.itemPositions.push_back(
            {-256, top + static_cast<f32>(titleHeight + static_cast<s32>(i) * rowHeight) * kScale});
    }
    return result;
}

bool LegendsDialogue::drawTopicsBackdrop(Canvas& canvas, const OptionMenu& menu,
                                         std::string_view title) const {
    if (!m_scroll || !m_titleText.ready()) {
        return false;
    }
    const Rect area = menu.backdropArea();
    // Native topic menus stretch the entire sheet, unlike the sliced long pages.
    canvas.draw(*m_scroll, area, {0, 0, 1, 1}, Color::white());
    m_titleText.draw(canvas, -256, static_cast<s32>(area.y + 10 * kScale), title,
                     {.scale = 1.5f * kScale, .color = Color::rgba(0, 0, 132)});
    return true;
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
