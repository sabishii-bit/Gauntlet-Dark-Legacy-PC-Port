#include "game/menu/ScrollBox.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "game/menu/OptionMenu.h"

namespace gdl::game {

std::vector<std::string> ScrollBox::splitLines(std::string_view page) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= page.size()) {
        const std::size_t end = page.find('\n', at);
        lines.emplace_back(
            page.substr(at, end == std::string_view::npos ? page.size() - at : end - at));
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

bool ScrollBox::open(RenderDevice& device, std::vector<std::string> pages, float scale,
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
void ScrollBox::showPage(std::size_t page) {
    m_page = page;
    m_lines = splitLines(m_pages[page]);
    std::int32_t textWidth = 0;
    for (const std::string& line : m_lines) {
        textWidth = std::max(textWidth, m_text->measure(line, m_scale));
    }
    const auto lineCount = static_cast<std::int32_t>(m_lines.size());
    const std::int32_t textHeight =
        lineCount * m_text->lineHeight(m_scale) + std::max(lineCount - 1, 0) * kLineSpacing;
    const std::int32_t narrowest = m_text->measure(m_prompt, kPromptScale) + kPromptExtra;
    const std::int32_t width = std::clamp(textWidth + kMargin, narrowest, kMaxWidth);
    const std::int32_t height = textHeight + kMargin;
    const std::int32_t x = kCentreX - width / 2;
    const std::int32_t y = kCentreY - height / 2;
    m_area = Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                  static_cast<float>(height)};
    m_textTop = y + kTextInset;
    m_promptY = m_textTop + textHeight + kPromptGap;
    m_hold = kHoldTicks;
}

void ScrollBox::step(std::int32_t ticks, std::uint32_t accepted) {
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
    if (!canBurn ||
        !m_fire.start(*m_device, m_area, *m_art.backdropImage, m_art.burnMasks, m_art.burnRing)) {
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
    std::int32_t y = m_textTop;
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
        canvas.draw(*m_art.button,
                    Rect{static_cast<float>(kButtonX), static_cast<float>(m_promptY),
                         static_cast<float>(kButtonSize), static_cast<float>(kButtonSize)});
    }
}

} // namespace gdl::game
