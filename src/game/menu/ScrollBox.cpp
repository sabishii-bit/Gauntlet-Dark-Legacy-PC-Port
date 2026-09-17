#include "game/menu/ScrollBox.h"

#include <algorithm>

#include "game/menu/OptionMenu.h"

namespace gdl::game {

std::vector<std::string> ScrollBox::splitLines(std::string_view page) {
    std::vector<std::string> lines;
    usize at = 0;
    while (at <= page.size()) {
        const usize end = page.find('\n', at);
        lines.emplace_back(page.substr(at, end == std::string_view::npos ? page.size() - at
                                                                         : end - at));
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

bool ScrollBox::open(RenderDevice& device, std::vector<std::string> pages, f32 scale,
                     std::string prompt) {
    close();
    if (m_text == nullptr || !m_text->ready() || pages.empty()) {
        return false;
    }
    m_device = &device;
    m_pages = std::move(pages);
    m_prompt = std::move(prompt);
    m_scale = scale;
    m_active = true;
    m_time = 0;
    showPage(0);
    return true;
}

void ScrollBox::close() {
    m_fire.reset();
    m_pages.clear();
    m_lines.clear();
    m_page = 0;
    m_active = false;
    m_device = nullptr;
}

/** Sizes the scroll to the page: the text plus its margins, no narrower than the prompt and
 * no wider than the screen, centred on the box's point. */
void ScrollBox::showPage(usize page) {
    m_page = page;
    m_lines = splitLines(m_pages[page]);
    s32 textWidth = 0;
    for (const std::string& line : m_lines) {
        textWidth = std::max(textWidth, m_text->measure(line, m_scale));
    }
    const auto lineCount = static_cast<s32>(m_lines.size());
    const s32 textHeight =
        lineCount * m_text->lineHeight(m_scale) + std::max(lineCount - 1, 0) * kLineSpacing;
    const s32 narrowest = m_text->measure(m_prompt, kPromptScale) + kPromptExtra;
    const s32 width = std::clamp(textWidth + kMargin, narrowest, kMaxWidth);
    const s32 height = textHeight + kMargin;
    const s32 x = kCentreX - width / 2;
    const s32 y = kCentreY - height / 2;
    m_area = Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(width),
                  static_cast<f32>(height)};
    m_textTop = y + kTextInset;
    m_promptY = m_textTop + textHeight + kPromptGap;
    m_hold = kHoldTicks;
}

void ScrollBox::step(s32 ticks, u32 accepted) {
    if (!m_active) {
        return;
    }
    m_time += ticks;
    if (m_fire.active()) {
        m_fire.step(ticks);
        if (!m_fire.active()) {
            close();
        }
        return;
    }
    if (m_hold > 0) {
        m_hold -= ticks;
        return;
    }
    if (accepted == 0) {
        return;
    }
    if (m_page + 1 < m_pages.size()) {
        showPage(m_page + 1);
    } else {
        finish();
    }
}

/** After the last page the scroll burns away; without the burn art it simply goes. */
void ScrollBox::finish() {
    const bool canBurn = m_device != nullptr && m_art.backdropImage != nullptr &&
                         !m_art.burnMasks.empty() && !m_art.burnRing.empty();
    if (!canBurn || !m_fire.start(*m_device, m_area, *m_art.backdropImage, m_art.burnMasks,
                                  m_art.burnRing)) {
        close();
    }
}

void ScrollBox::prepare(RenderDevice& device) {
    if (m_fire.active()) {
        m_fire.prepare(device);
    }
}

void ScrollBox::draw(Canvas& canvas) const {
    if (!m_active || m_text == nullptr) {
        return;
    }
    // The words are gone before the scroll burns; only the fire is left to see.
    if (m_fire.active()) {
        m_fire.draw(canvas);
        return;
    }
    if (m_art.backdrop != nullptr) {
        canvas.draw(*m_art.backdrop, m_area);
    }
    TextStyle style;
    style.scale = m_scale;
    style.color = kTextColor;
    s32 y = m_textTop;
    for (const std::string& line : m_lines) {
        if (!line.empty()) {
            m_text->draw(canvas, -kCentreX, y, line, style);
        }
        y += m_text->lineHeight(m_scale) + kLineSpacing;
    }
    // The prompt glows under the text, its button icon in the gap the words leave for it.
    TextStyle glow;
    glow.scale = kPromptScale;
    glow.color = kGlowColor.withAlpha(pulseOpacity(m_time, kGlowPulseTicks, kGlowHoldTicks));
    glow.texture = m_art.glow;
    glow.expand = OptionMenu::kGlowExpand;
    m_text->draw(canvas, -kCentreX, m_promptY, m_prompt, glow);
    TextStyle plain;
    plain.scale = kPromptScale;
    m_text->draw(canvas, -kCentreX, m_promptY, m_prompt, plain);
    if (m_art.button != nullptr) {
        canvas.draw(*m_art.button, Rect{static_cast<f32>(kButtonX), static_cast<f32>(m_promptY),
                                        static_cast<f32>(kButtonSize),
                                        static_cast<f32>(kButtonSize)});
    }
}

} // namespace gdl::game
