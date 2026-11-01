#include "game/menu/ScrollBox.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/menu/OptionMenu.h"

namespace gdl::game {

std::vector<std::string> ScrollBox::splitLines(std::string_view page) {
    std::vector<std::string> lines;
    usize at = 0;
    while (at <= page.size()) {
        const usize end = page.find('\n', at);
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

bool ScrollBox::open(RenderDevice& device, std::vector<std::string> pages, f32 scale,
                     std::string prompt) {
    close();
    if ((m_presentation ? !m_presentation->text().ready()
                        : m_text == nullptr || !m_text->ready()) ||
        pages.empty()) {
        return false;
    }
    m_sim.device = &device;
    m_sim.pages = std::move(pages);
    m_sim.prompt = std::move(prompt);
    m_sim.scale = scale;
    m_sim.active = true;
    m_sim.time = 0;
    showPage(0);
    return true;
}

void ScrollBox::close() {
    m_fire.reset();
    m_sim.pages.clear();
    m_sim.layout = {};
    m_sim.page = 0;
    m_sim.active = false;
    m_sim.device = nullptr;
}

/** Sizes the scroll to the page: the text plus its margins, no narrower than the prompt and
 * no wider than the screen, centred on the box's point. */
void ScrollBox::showPage(usize page) {
    m_sim.page = page;
    m_sim.layout = layout(m_sim.pages[page], m_sim.scale, m_sim.prompt);
    m_sim.hold = kHoldTicks;
}

ScrollBox::PageLayout ScrollBox::layout(std::string_view page, f32 scale,
                                        std::string_view prompt) const {
    if (m_presentation) {
        return m_presentation->layout(page, scale, prompt);
    }
    PageLayout result;
    result.lines = splitLines(page);
    s32 textWidth = 0;
    for (const std::string& line : result.lines) {
        textWidth = std::max(textWidth, m_text->measure(line, scale));
    }
    const auto lineCount = static_cast<s32>(result.lines.size());
    const s32 textHeight =
        lineCount * m_text->lineHeight(scale) + std::max(lineCount - 1, 0) * kLineSpacing;
    const s32 narrowest = m_text->measure(prompt, kPromptScale) + kPromptExtra;
    const s32 width = std::clamp(textWidth + kMargin, narrowest, kMaxWidth);
    const s32 height = textHeight + kMargin;
    const s32 x = kCentreX - width / 2;
    const s32 y = kCentreY - height / 2;
    result.area = Rect{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(width),
                       static_cast<f32>(height)};
    result.textTop = y + kTextInset;
    result.promptY = result.textTop + textHeight + kPromptGap;
    return result;
}

void ScrollBox::step(s32 ticks, u32 accepted) {
    if (!m_sim.active) {
        return;
    }
    m_sim.time += ticks;
    if (m_fire.active()) {
        m_fire.step(ticks);
        if (!m_fire.active()) {
            close();
        }
        return;
    }
    if (m_sim.hold > 0) {
        m_sim.hold -= ticks;
        return;
    }
    if (accepted == 0) {
        return;
    }
    if (m_sim.page + 1 < m_sim.pages.size()) {
        showPage(m_sim.page + 1);
    } else {
        finish();
    }
}

/** After the last page the scroll burns away; without the burn art it simply goes. */
void ScrollBox::finish() {
    if (m_presentation) {
        close();
        return;
    }
    const bool canBurn = m_sim.device != nullptr && m_art.backdropImage != nullptr &&
                         !m_art.burnMasks.empty() && !m_art.burnRing.empty();
    if (!canBurn || !m_fire.start(*m_sim.device, m_sim.layout.area, *m_art.backdropImage,
                                  m_art.burnMasks, m_art.burnRing)) {
        close();
    }
}

void ScrollBox::prepare(RenderDevice& device) {
    if (m_fire.active()) {
        m_fire.prepare(device);
    }
}

void ScrollBox::draw(Canvas& canvas) const {
    if (!m_sim.active || (m_text == nullptr && m_presentation == nullptr)) {
        return;
    }
    // The words are gone before the scroll burns; only the fire is left to see.
    if (m_fire.active()) {
        m_fire.draw(canvas);
        return;
    }
    drawWords(canvas, m_sim.layout, m_sim.scale, m_sim.prompt, promptAlpha());
}

u8 ScrollBox::promptAlpha() const {
    return pulseOpacity(m_sim.time, kGlowPulseTicks, kGlowHoldTicks);
}

void ScrollBox::drawWords(Canvas& canvas, const PageLayout& page, f32 scale,
                          std::string_view prompt, u8 alpha) const {
    if (m_presentation) {
        m_presentation->draw(canvas, page, scale, prompt, alpha);
        return;
    }
    if (m_art.backdrop != nullptr) {
        canvas.draw(*m_art.backdrop, page.area);
    }
    TextStyle style;
    style.scale = scale;
    style.color = kTextColor;
    s32 y = page.textTop;
    for (const std::string& line : page.lines) {
        if (!line.empty()) {
            m_text->draw(canvas, -kCentreX, y, line, style);
        }
        y += m_text->lineHeight(scale) + kLineSpacing;
    }
    // The prompt glows under the text, its button icon in the gap the words leave for it.
    TextStyle glow;
    glow.scale = kPromptScale;
    glow.color = kGlowColor.withAlpha(alpha);
    glow.texture = m_art.glow;
    glow.expand = OptionMenu::kGlowExpand;
    m_text->draw(canvas, -kCentreX, page.promptY, prompt, glow);
    TextStyle plain;
    plain.scale = kPromptScale;
    m_text->draw(canvas, -kCentreX, page.promptY, prompt, plain);
    if (m_art.button != nullptr) {
        canvas.draw(*m_art.button,
                    Rect{static_cast<f32>(kButtonX), static_cast<f32>(page.promptY),
                         static_cast<f32>(kButtonSize), static_cast<f32>(kButtonSize)});
    }
}

bool ScrollBox::preloadBurn(RenderDevice& device) {
    m_burnFrames.clear();
    if (m_art.backdropImage == nullptr || m_art.burnMasks.empty() || m_art.burnRing.empty()) {
        return false;
    }
    for (usize i = 0; i < static_cast<usize>(BurnDialogueScroll::kFrameCount); ++i) {
        const auto* mask = m_art.burnMasks[std::min(i, m_art.burnMasks.size() - 1)];
        if (mask == nullptr || mask->pixels.empty()) {
            m_burnFrames.clear();
            return false;
        }
        const auto image = BurnDialogueScroll::composite(*m_art.backdropImage, *mask);
        m_burnFrames.push_back(device.createTexture(
            {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
            image.pixels));
    }
    return true;
}

bool ScrollBox::acceptsFrame(s32 frame) const {
    return m_text != nullptr && m_text->ready() && m_art.backdrop != nullptr &&
           (frame == -1 || (frame >= 0 && static_cast<usize>(frame) < m_burnFrames.size()));
}

void ScrollBox::drawPage(Canvas& canvas, const PageLayout& page, f32 scale, std::string_view prompt,
                         u8 alpha, s32 burnFrame) const {
    if (!acceptsFrame(burnFrame)) {
        return;
    }
    if (burnFrame < 0) {
        drawWords(canvas, page, scale, prompt, alpha);
        return;
    }
    const auto frame = static_cast<usize>(burnFrame);
    canvas.draw(*m_burnFrames[frame], page.area);
    if (const auto* ring = m_art.burnRing[std::min(frame, m_art.burnRing.size() - 1)]) {
        canvas.draw(*ring, page.area);
    }
}

void ScrollBox::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
    m_fire.saveLocal(saved.m_fire);
}

bool ScrollBox::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_localBinding &&
           m_fire.canRestoreLocal(saved.m_fire);
}

bool ScrollBox::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_fire.restoreLocal(saved.m_fire);
    return true;
}

bool ScrollBox::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("scroll-box-native-v1");
    const auto& [backdrop, backdropImage, glow, button, masks, ring] = m_art;
    digest.resource(backdrop);
    digest.resource(backdropImage);
    digest.resource(glow);
    digest.resource(button);
    digest.wide(masks.size());
    for (const auto* mask : masks) {
        digest.resource(mask);
    }
    digest.wide(ring.size());
    for (const auto* texture : ring) {
        digest.resource(texture);
    }
    digest.flag(m_text != nullptr);
    digest.flag(m_presentation != nullptr);
    if (m_text) {
        digest.value(*m_text);
    }
    const auto& [device, pages, layout, prompt, scale, page, hold, time, active] =
        saved ? saved->m_state : m_sim;
    // Device presence selects the burn fallback. Its allocation identity and
    // the preloaded/uploaded frames do not affect the simulation.
    digest.flag(device != nullptr);
    digest.values(pages, layout, prompt, scale, page, hold, time, active);
    if (saved) {
        m_fire.appendLocalDigest(digest, saved->m_fire);
    } else {
        m_fire.appendNativeDigest(digest);
    }
    return digest.good();
}

bool ScrollBox::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool ScrollBox::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

} // namespace gdl::game
