#include "game/widgets/BurnDialogueScroll.h"

#include <algorithm>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

} // namespace

bool BurnDialogueScroll::ready(const Image* scroll, std::span<const Image* const> masks,
                               std::span<const Texture* const> ring) {
    const auto image = [](const Image* value) {
        return value != nullptr && value->width != 0 && value->height != 0 &&
               value->pixels.size() % 4 == 0 &&
               value->pixels.size() / 4 == u64{value->width} * value->height;
    };
    if (!image(scroll) || masks.empty() || ring.empty() || !image(masks.front())) {
        return false;
    }
    return std::ranges::all_of(masks,
                               [&](const Image* mask) {
                                   return image(mask) && mask->width == masks.front()->width &&
                                          mask->height == masks.front()->height;
                               }) &&
           std::ranges::all_of(ring, [](const Texture* texture) {
               return texture != nullptr && texture->width() != 0 && texture->height() != 0;
           });
}

bool BurnDialogueScroll::start(RenderDevice& /*unused*/, const Rect& area, const Image& scroll,
                               std::vector<const Image*> masks, std::vector<const Texture*> ring) {
    reset();
    if (scroll.pixels.empty() || masks.empty() || ring.empty() || masks.front() == nullptr) {
        log::warn("Burning scroll: missing the scroll image or its frames");
        return false;
    }
    for (const Image* mask : masks) {
        if (mask == nullptr || mask->width != masks.front()->width ||
            mask->height != masks.front()->height) {
            log::warn("Burning scroll: burn frames must share one size");
            return false;
        }
    }
    // The burn still completes when individual flame frames are absent. The
    // stricter ready() check is for callers requiring the complete artwork.
    NativeStateDigest source;
    source.string("burn-scroll-source-v1");
    source.fields<3>(scroll);
    m_sim.art = std::make_shared<const Artwork>(
        Artwork{scroll, std::move(masks), std::move(ring), source.finish()});
    m_sim.area = area;
    m_sim.active = true;
    return true;
}

void BurnDialogueScroll::reset() {
    m_sim = {};
}

void BurnDialogueScroll::step(s32 ticks) {
    if (!m_sim.active) {
        return;
    }
    m_sim.timer += ticks;
    if (frame() >= kFrameCount) {
        m_sim.active = false;
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

u8 BurnDialogueScroll::compositeAlpha(u32 x, u32 y) const {
    if (!m_sim.art) {
        return 0;
    }
    const auto& art = *m_sim.art;
    const auto index =
        static_cast<usize>(std::clamp(frame(), 0, static_cast<s32>(art.masks.size()) - 1));
    const Image& mask = *art.masks[index];
    if (mask.width == 0 || mask.height == 0 || mask.pixels.empty()) {
        return art.source.pixel(x, y).a;
    }
    return maskAlpha(art.source, mask, x, y) == 0 ? 0 : art.source.pixel(x, y).a;
}

void BurnDialogueScroll::prepare(RenderDevice& device) {
    if (!m_sim.active || !m_sim.art) {
        return;
    }
    const auto& art = *m_sim.art;
    if (m_uploadedArt == m_sim.art && m_uploadedFrame == frame() && m_device == &device) {
        return;
    }
    const auto index =
        static_cast<usize>(std::clamp(frame(), 0, static_cast<s32>(art.masks.size()) - 1));
    const Image pixels = composite(art.source, *art.masks[index]);
    if (!m_texture || m_texture->width() != pixels.width || m_texture->height() != pixels.height ||
        m_device != &device) {
        m_texture =
            device.createTexture(TextureDesc{pixels.width, pixels.height, TextureFilter::Linear,
                                             TextureWrap::ClampToEdge},
                                 pixels.pixels);
    } else {
        device.updateTexture(*m_texture, pixels.pixels);
    }
    m_device = &device;
    m_uploadedArt = m_sim.art;
    m_uploadedFrame = frame();
}

void BurnDialogueScroll::draw(Canvas& canvas) const {
    if (!m_sim.active || !m_texture || m_uploadedArt != m_sim.art || m_uploadedFrame != frame()) {
        return;
    }
    canvas.draw(*m_texture, m_sim.area);
    const auto& ring = m_sim.art->ring;
    const auto ringFrame =
        static_cast<usize>(std::min<s32>(frame(), static_cast<s32>(ring.size()) - 1));
    if (ring[ringFrame] != nullptr) {
        canvas.draw(*ring[ringFrame], m_sim.area);
    }
}

void BurnDialogueScroll::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
}

bool BurnDialogueScroll::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && saved.m_binding.lock() == m_localBinding;
}

bool BurnDialogueScroll::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

bool BurnDialogueScroll::appendSimulationDigest(NativeStateDigest& digest,
                                                const Simulation& state) {
    digest.string("burn-scroll-native-v1");
    const auto& [art, area, timer, active] = state;
    digest.flag(art != nullptr);
    if (art) {
        const auto& [source, masks, ring, sourceIdentity] = *art;
        // Artwork owns an immutable copy. Its pixels were fingerprinted once at
        // construction; frame uploads and compositing are presentation caches.
        digest.values(source.width, source.height, sourceIdentity);
        digest.wide(masks.size());
        for (const auto* mask : masks) {
            digest.resource(mask);
        }
        digest.wide(ring.size());
        for (const auto* texture : ring) {
            digest.resource(texture);
        }
    }
    digest.values(area, timer, active);
    return digest.good();
}

bool BurnDialogueScroll::appendNativeDigest(NativeStateDigest& digest) const {
    return appendSimulationDigest(digest, m_sim);
}

bool BurnDialogueScroll::appendLocalDigest(NativeStateDigest& digest,
                                           const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendSimulationDigest(digest, saved.m_state);
}

} // namespace gdl::game
