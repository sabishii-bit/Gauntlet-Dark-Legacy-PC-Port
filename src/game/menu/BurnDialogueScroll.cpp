#include "game/menu/BurnDialogueScroll.h"

#include <algorithm>
#include <cstddef>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace

bool BurnDialogueScroll::start(RenderDevice& device, const Rect& area, const Image& scroll,
                               std::vector<const Image*> masks, std::vector<const Texture*> ring) {
    reset();
    if (scroll.pixels.empty() || masks.empty() || ring.empty()) {
        log::warn("Burning scroll: missing the scroll image or its frames");
        return false;
    }
    for (const Image* mask : masks) {
        if (mask == nullptr || mask->width != masks[0]->width || mask->height != masks[0]->height) {
            log::warn("Burning scroll: burn frames must share one size");
            return false;
        }
    }
    m_area = area;
    m_source = scroll;
    m_composite = scroll;
    m_masks = std::move(masks);
    m_ring = std::move(ring);
    m_maskWidth = m_masks[0]->width;
    m_maskHeight = m_masks[0]->height;
    m_texture = device.createTexture(TextureDesc{m_source.width, m_source.height,
                                                 TextureFilter::Linear, TextureWrap::ClampToEdge},
                                     m_source.pixels);
    m_timer = 0;
    m_cutFrame = -1;
    m_uploadedFrame = -1;
    m_active = true;
    cutOut(0);
    return true;
}

void BurnDialogueScroll::reset() {
    m_active = false;
    m_texture.reset();
    m_masks.clear();
    m_ring.clear();
    m_timer = 0;
    m_cutFrame = -1;
    m_uploadedFrame = -1;
}

void BurnDialogueScroll::step(int ticks) {
    if (!m_active) {
        return;
    }
    m_timer += ticks;
    if (frame() >= kFrameCount) {
        m_active = false;
        return;
    }
    if (m_cutFrame != frame()) {
        cutOut(frame());
    }
}

/** The mask's alpha under a scroll texel, filtered as a blit stretched over the scroll is. */
float BurnDialogueScroll::maskAlpha(const Image& mask, unsigned int x, unsigned int y) const {
    const auto maxU = static_cast<float>(m_maskWidth - 1);
    const auto maxV = static_cast<float>(m_maskHeight - 1);
    const float u = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(m_maskWidth) /
                                       static_cast<float>(m_source.width) -
                                   0.5f,
                               0.0f, maxU);
    const float v = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(m_maskHeight) /
                                       static_cast<float>(m_source.height) -
                                   0.5f,
                               0.0f, maxV);
    const auto x0 = static_cast<unsigned int>(u);
    const auto y0 = static_cast<unsigned int>(v);
    const unsigned int x1 = std::min(x0 + 1, m_maskWidth - 1);
    const unsigned int y1 = std::min(y0 + 1, m_maskHeight - 1);
    const float fx = u - static_cast<float>(x0);
    const float fy = v - static_cast<float>(y0);
    const float top = lerp(mask.pixel(x0, y0).a, mask.pixel(x1, y0).a, fx);
    const float bottom = lerp(mask.pixel(x0, y1).a, mask.pixel(x1, y1).a, fx);
    return lerp(top, bottom, fy);
}

/** Cuts the scroll out wherever `frame` of the mask is fully transparent. */
void BurnDialogueScroll::cutOut(int frame) {
    const Image& mask = *m_masks[static_cast<std::size_t>(
        std::min<int>(frame, static_cast<int>(m_masks.size()) - 1))];
    for (unsigned int y = 0; y < m_source.height; ++y) {
        for (unsigned int x = 0; x < m_source.width; ++x) {
            Color color = m_source.pixel(x, y);
            if (maskAlpha(mask, x, y) == 0.0f) {
                color.a = 0;
            }
            m_composite.setPixel(x, y, color);
        }
    }
    m_cutFrame = frame;
}

void BurnDialogueScroll::prepare(RenderDevice& device) {
    if (!m_active || !m_texture || m_uploadedFrame == m_cutFrame) {
        return;
    }
    device.updateTexture(*m_texture, m_composite.pixels);
    m_uploadedFrame = m_cutFrame;
}

void BurnDialogueScroll::draw(Canvas& canvas) const {
    if (!m_active || !m_texture) {
        return;
    }
    canvas.draw(*m_texture, m_area);
    const auto ringFrame =
        static_cast<std::size_t>(std::min<int>(frame(), static_cast<int>(m_ring.size()) - 1));
    if (m_ring[ringFrame] != nullptr) {
        canvas.draw(*m_ring[ringFrame], m_area);
    }
}

} // namespace gdl::game
