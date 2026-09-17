#pragma once

#include <memory>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {

/**
 * Burns a menu backdrop away. Each mask frame is what is left of the scroll: its transparent
 * texels are burnt out, the hole grows frame by frame, and a ring of flame (the colour frames)
 * is drawn over the front. Runs on the original's tick clock: one frame every two ticks.
 */
class FireScroll {
public:
    static constexpr s32 kFrameCount = 21;
    static constexpr s32 kTicksPerFrame = 2;

    /**
     * Starts burning `scroll` over `area`. `masks` are the burn frames (alpha zero is burnt),
     * stretched over the scroll with filtering; `ring` the flame frames drawn on top. Keeps a
     * GPU copy of the scroll.
     */
    bool start(RenderDevice& device, const Rect& area, const Image& scroll,
               std::vector<const Image*> masks, std::vector<const Texture*> ring);

    void reset();
    bool active() const { return m_active; }
    s32 frame() const { return m_timer / kTicksPerFrame; }

    /** Advances the effect; it ends once the last frame has played. */
    void step(s32 ticks);

    /** Uploads the current frame's cut-out scroll; call after beginFrame and before drawing. */
    void prepare(RenderDevice& device);

    void draw(Canvas& canvas) const;

    /** Alpha of the composited scroll at a texel, for tests. */
    u8 compositeAlpha(u32 x, u32 y) const { return m_composite.pixel(x, y).a; }

private:
    void cutOut(s32 frame);
    f32 maskAlpha(const Image& mask, u32 x, u32 y) const;

    bool m_active = false;
    Rect m_area;
    Image m_source;
    Image m_composite;
    std::unique_ptr<Texture> m_texture;
    std::vector<const Image*> m_masks;
    std::vector<const Texture*> m_ring;
    u32 m_maskWidth = 0;
    u32 m_maskHeight = 0;
    s32 m_timer = 0;
    s32 m_cutFrame = -1;
    s32 m_uploadedFrame = -1;
};

} // namespace gdl::game
