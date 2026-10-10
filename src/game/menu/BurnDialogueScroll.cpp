#include "game/menu/BurnDialogueScroll.h"

#include <algorithm>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

f32 lerp(f32 a, f32 b, f32 t) {
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

void BurnDialogueScroll::step(s32 ticks) {
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
f32 BurnDialogueScroll::maskAlpha(const Image& source, const Image& mask, u32 x, u32 y) {
    const auto maxU = static_cast<f32>(mask.width - 1);
    const auto maxV = static_cast<f32>(mask.height - 1);
    const f32 u = std::clamp((static_cast<f32>(x) + 0.5f) * static_cast<f32>(mask.width) /
                                     static_cast<f32>(source.width) -
                                 0.5f,
                             0.0f, maxU);
    const f32 v = std::clamp((static_cast<f32>(y) + 0.5f) * static_cast<f32>(mask.height) /
                                     static_cast<f32>(source.height) -
                                 0.5f,
                             0.0f, maxV);
    const auto x0 = static_cast<u32>(u);
    const auto y0 = static_cast<u32>(v);
    const u32 x1 = std::min(x0 + 1, mask.width - 1);
    const u32 y1 = std::min(y0 + 1, mask.height - 1);
    const f32 fx = u - static_cast<f32>(x0);
    const f32 fy = v - static_cast<f32>(y0);
    const f32 top = lerp(mask.pixel(x0, y0).a, mask.pixel(x1, y0).a, fx);
    const f32 bottom = lerp(mask.pixel(x0, y1).a, mask.pixel(x1, y1).a, fx);
    return lerp(top, bottom, fy);
}

/** Cuts the scroll out wherever `frame` of the mask is fully transparent. */
void BurnDialogueScroll::cutOut(s32 frame) {
    const Image& mask =
        *m_masks[static_cast<usize>(std::min<s32>(frame, static_cast<s32>(m_masks.size()) - 1))];
    m_composite = composite(m_source, mask);
    m_cutFrame = frame;
}

Image BurnDialogueScroll::composite(const Image& scroll, const Image& mask) {
    Image result = scroll;
    if (mask.width == 0 || mask.height == 0 || mask.pixels.empty()) {
        return result;
    }
    for (u32 y = 0; y < scroll.height; ++y) {
        for (u32 x = 0; x < scroll.width; ++x) {
            Color color = scroll.pixel(x, y);
            if (maskAlpha(scroll, mask, x, y) == 0.0f) {
                color.a = 0;
            }
            result.setPixel(x, y, color);
        }
    }
    return result;
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
        static_cast<usize>(std::min<s32>(frame(), static_cast<s32>(m_ring.size()) - 1));
    if (m_ring[ringFrame] != nullptr) {
        canvas.draw(*m_ring[ringFrame], m_area);
    }
}

} // namespace gdl::game
