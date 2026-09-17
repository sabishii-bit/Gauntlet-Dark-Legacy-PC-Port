#include "game/menu/FireScroll.h"

#include <algorithm>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

} // namespace

bool FireScroll::start(RenderDevice& device, const Rect& area, const Image& scroll,
                       std::vector<const Image*> masks, std::vector<const Texture*> ring) {
    reset();
    if (scroll.pixels.empty() || masks.empty() || ring.empty()) {
        log::warn("Fire scroll: missing the scroll image or its frames");
        return false;
    }
    for (const Image* mask : masks) {
        if (mask == nullptr || mask->width != masks[0]->width || mask->height != masks[0]->height) {
            log::warn("Fire scroll: burn frames must share one size");
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

void FireScroll::reset() {
    m_active = false;
    m_texture.reset();
    m_masks.clear();
    m_ring.clear();
    m_timer = 0;
    m_cutFrame = -1;
    m_uploadedFrame = -1;
}

void FireScroll::step(s32 ticks) {
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
f32 FireScroll::maskAlpha(const Image& mask, u32 x, u32 y) const {
    const auto maxU = static_cast<f32>(m_maskWidth - 1);
    const auto maxV = static_cast<f32>(m_maskHeight - 1);
    const f32 u = std::clamp((static_cast<f32>(x) + 0.5f) * static_cast<f32>(m_maskWidth) /
                                     static_cast<f32>(m_source.width) -
                                 0.5f,
                             0.0f, maxU);
    const f32 v = std::clamp((static_cast<f32>(y) + 0.5f) * static_cast<f32>(m_maskHeight) /
                                     static_cast<f32>(m_source.height) -
                                 0.5f,
                             0.0f, maxV);
    const auto x0 = static_cast<u32>(u);
    const auto y0 = static_cast<u32>(v);
    const u32 x1 = std::min(x0 + 1, m_maskWidth - 1);
    const u32 y1 = std::min(y0 + 1, m_maskHeight - 1);
    const f32 fx = u - static_cast<f32>(x0);
    const f32 fy = v - static_cast<f32>(y0);
    const f32 top = lerp(mask.pixel(x0, y0).a, mask.pixel(x1, y0).a, fx);
    const f32 bottom = lerp(mask.pixel(x0, y1).a, mask.pixel(x1, y1).a, fx);
    return lerp(top, bottom, fy);
}

/** Cuts the scroll out wherever `frame` of the mask is fully transparent. */
void FireScroll::cutOut(s32 frame) {
    const Image& mask =
        *m_masks[static_cast<usize>(std::min<s32>(frame, static_cast<s32>(m_masks.size()) - 1))];
    for (u32 y = 0; y < m_source.height; ++y) {
        for (u32 x = 0; x < m_source.width; ++x) {
            Color color = m_source.pixel(x, y);
            if (maskAlpha(mask, x, y) == 0.0f) {
                color.a = 0;
            }
            m_composite.setPixel(x, y, color);
        }
    }
    m_cutFrame = frame;
}

void FireScroll::prepare(RenderDevice& device) {
    if (!m_active || !m_texture || m_uploadedFrame == m_cutFrame) {
        return;
    }
    device.updateTexture(*m_texture, m_composite.pixels);
    m_uploadedFrame = m_cutFrame;
}

void FireScroll::draw(Canvas& canvas) const {
    if (!m_active || !m_texture) {
        return;
    }
    canvas.draw(*m_texture, m_area);
    const auto ringFrame =
        static_cast<usize>(std::min<s32>(frame(), static_cast<s32>(m_ring.size()) - 1));
    if (m_ring[ringFrame] != nullptr) {
        canvas.draw(*m_ring[ringFrame], m_area);
    }
}

} // namespace gdl::game
