#include "expansions/n64/menu/LegendsDialogue.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64Scroll.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::game {

namespace {

/** The frame's middle column, where e086098c centres the Tower captions and the topic menu
 * stands its column. */
constexpr f32 kFrameMiddle = LegendsN64Frame::kWidth / 2;
/** Tower e0860620's caption row. */
constexpr s32 kCaptionTop = 30;

/** e00717a0/e0071a10's long-page scroll: 350 wide at (40, 40) of the frame, sixteen rows taller
 * than its text, which starts six rows under its top and 60 in (the width's 12/70, rounded). */
constexpr f32 kScrollLeft = 40;
constexpr f32 kScrollTop = 40;
constexpr f32 kScrollWidth = 350;
constexpr s32 kScrollPadding = 16;
constexpr s32 kTextDrop = 6;
constexpr f32 kTextInset = 60;
/** Rows between a page's last line and the port's prompt under it. */
constexpr s32 kPromptGap = 3;

/** e0046000 / e0041894's topic menu: the font8 title at 1.5 with a 15-row gap under it, the
 * rows five apart, the TEXTSCROLL 30 columns and 10 rows beyond them, the whole centred in the
 * frame's top three quarters. */
constexpr f32 kTitleScale = 1.5f;
constexpr s32 kTitleGap = 15;
constexpr s32 kRowGap = 5;
constexpr s32 kMarginColumns = 30;
constexpr s32 kMarginRows = 10;
constexpr f32 kTopicsShare = 0.75f;

/** The x that centres a canvas line on the frame's middle column, as TextPainter and OptionMenu
 * centre a line on minus its x. */
s32 canvasMiddle() {
    return -static_cast<s32>(std::lround(LegendsN64Frame::toCanvas({kFrameMiddle, 0}).x));
}

} // namespace

bool LegendsDialogue::load(RenderDevice& device, const std::filesystem::path& nativeRoot) {
    m_text.setFont(nullptr, nullptr);
    m_titleText.setFont(nullptr, nullptr);
    m_fontSheet.reset();
    m_titleFontSheet.reset();
    m_scroll.reset();
    m_menuTextures = {};
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
        m_menuTextures.backdrop = m_scroll.get();
    } catch (const std::exception& error) {
        log::warn("N64 dialogue: {}", error.what());
        return false;
    }
    // e0041894's font8 title serves the topic menu alone: without it nativeTopics
    // leaves the topics to the shared menu, and the popups keep this font and scroll.
    if (const auto titleFont = files.find("Font/bigfont.fnt")) {
        try {
            auto decoded = formats::LegendsN64Font::parse(readFile(*titleFont));
            m_titleFont = std::move(decoded.font);
            m_titleFontSheet =
                device.createTexture({decoded.atlas.width, decoded.atlas.height,
                                      TextureFilter::Linear, TextureWrap::ClampToEdge},
                                     decoded.atlas.pixels);
            m_titleText.setFont(&m_titleFont, m_titleFontSheet.get());
        } catch (const std::exception& error) {
            log::warn("N64 dialogue title font: {}", error.what());
        }
    }
    return true;
}

void LegendsDialogue::drawCeremonyCaption(Canvas& canvas, std::string_view text,
                                          f32 opacity) const {
    if (text.empty() || opacity <= 0 || !m_text.ready()) {
        return;
    }
    // Tower e0860620 selects font5, scale1, y30. e086098c centres each
    // caption at native x208 and fades over .3 seconds at both ends.
    const CanvasFrame frame{canvas, LegendsN64Frame::transform()};
    m_text.draw(canvas, -static_cast<s32>(kFrameMiddle), kCaptionTop, text,
                {.color = Color::white().withAlpha(
                     static_cast<u8>(std::clamp(opacity, 0.0f, 1.0f) * 255))});
}

std::optional<MenuDefinition>
LegendsDialogue::nativeTopics(std::string_view title, std::span<const MenuItem> topics) const {
    if (!m_text.ready() || !m_titleText.ready() || !m_scroll || topics.empty()) {
        return {};
    }
    // Laid out in the frame's units, the font5 rows measured at 1, and placed
    // on the canvas through the frame. OptionMenu draws the rows' glyphs at one
    // scale, so they alone keep kTopicGlyphScale.
    const s32 titleHeight =
        static_cast<s32>(static_cast<f32>(m_titleFont.height()) * kTitleScale) + kTitleGap;
    const s32 rowHeight = m_font.height() + kRowGap;
    const s32 contentHeight = titleHeight + static_cast<s32>(topics.size()) * rowHeight - kRowGap;
    s32 width = m_titleText.measure(title, kTitleScale);
    for (const auto& topic : topics) {
        width = std::max(width, m_text.measure(topic.text, 1));
    }
    // The halves are whole native pixels.
    const s32 halfContentHeight = contentHeight / 2;
    const s32 halfWidth = width / 2;
    const f32 top =
        LegendsN64Frame::kHeight * kTopicsShare / 2 - static_cast<f32>(halfContentHeight);
    const Vec2 corner =
        LegendsN64Frame::toCanvas({kFrameMiddle - static_cast<f32>(halfWidth + kMarginColumns),
                                   top - static_cast<f32>(kMarginRows)});
    MenuDefinition result;
    result.items.assign(topics.begin(), topics.end());
    result.scale = kTopicGlyphScale;
    result.showCursor = false;
    result.colors = {Color::black(), Color::rgba(0, 132, 0), Color::rgba(0, 0, 0, 0)};
    result.backdrop = "TEXTSCROLL";
    result.backdropX = static_cast<s32>(corner.x);
    result.backdropY = static_cast<s32>(corner.y);
    result.backdropWidth = static_cast<s32>(static_cast<f32>((halfWidth + kMarginColumns) * 2) *
                                            LegendsN64Frame::kColumn);
    result.backdropHeight =
        static_cast<s32>(static_cast<f32>(contentHeight + kMarginRows * 2) * LegendsN64Frame::kRow);
    const auto middle = static_cast<f32>(canvasMiddle());
    result.itemPositions.reserve(topics.size());
    for (usize i = 0; i < topics.size(); ++i) {
        const f32 row = top + static_cast<f32>(titleHeight + static_cast<s32>(i) * rowHeight);
        result.itemPositions.emplace_back(middle, LegendsN64Frame::toCanvas({kFrameMiddle, row}).y);
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
    // The title stands the scroll's margin of frame rows under its top.
    canvas.draw(*m_scroll, area, {0, 0, 1, 1}, Color::white());
    m_titleText.draw(
        canvas, canvasMiddle(),
        static_cast<s32>(area.y + static_cast<f32>(kMarginRows) * LegendsN64Frame::kRow), title,
        {.scale = kTitleScale * kTopicGlyphScale, .color = Color::rgba(0, 0, 132)});
    return true;
}

DialoguePage LegendsDialogue::layout(std::string_view page, f32 scale,
                                     std::string_view prompt) const {
    DialoguePage result;
    result.lines = ScrollBox::splitLines(page);
    const s32 lineHeight = m_text.lineHeight(scale);
    const auto count = static_cast<s32>(result.lines.size());
    // e00717a0/e0071a10: font5 on the 350-wide long-page scroll, in frame
    // units; the port's prompt takes a line under the text.
    const s32 height = count * lineHeight + kScrollPadding + (prompt.empty() ? 0 : lineHeight);
    result.area = {kScrollLeft, kScrollTop, kScrollWidth, static_cast<f32>(height)};
    result.textTop = static_cast<s32>(kScrollTop) + kTextDrop;
    result.promptY = result.textTop + count * lineHeight + kPromptGap;
    return result;
}

void LegendsDialogue::draw(Canvas& canvas, const DialoguePage& page, f32 scale,
                           std::string_view prompt, u8 promptAlpha) const {
    if (!m_scroll || !m_text.ready()) {
        return;
    }
    // The page is the N64's: drawn in its frame, the authored caps eight rows
    // tall and only the rows between stretched to the text.
    const CanvasFrame frame{canvas, LegendsN64Frame::transform()};
    drawLegendsN64Scroll(canvas, *m_scroll, page.area);
    TextStyle ink{.scale = scale, .color = Color::black()};
    const auto left = static_cast<s32>(page.area.x + kTextInset);
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
