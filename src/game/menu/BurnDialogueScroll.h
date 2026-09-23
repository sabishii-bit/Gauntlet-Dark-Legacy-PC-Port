#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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
class BurnDialogueScroll {
public:
    static constexpr int kFrameCount = 21;
    static constexpr int kTicksPerFrame = 2;

    /**
     * Starts burning `scroll` over `area`. `masks` are the burn frames (alpha zero is burnt),
     * stretched over the scroll with filtering; `ring` the flame frames drawn on top. Keeps a
     * GPU copy of the scroll.
     */
    bool start(RenderDevice& device, const Rect& area, const Image& scroll,
               std::vector<const Image*> masks, std::vector<const Texture*> ring);

    void reset();
    bool active() const { return m_active; }
    int frame() const { return m_timer / kTicksPerFrame; }

    /** Advances the effect; it ends once the last frame has played. */
    void step(int ticks);

    /** Uploads the current frame's cut-out scroll; call after beginFrame and before drawing. */
    void prepare(RenderDevice& device);

    void draw(Canvas& canvas) const;

    /** Alpha of the composited scroll at a texel, for tests. */
    std::uint8_t compositeAlpha(unsigned int x, unsigned int y) const {
        return m_composite.pixel(x, y).a;
    }

private:
    void cutOut(int frame);
    float maskAlpha(const Image& mask, unsigned int x, unsigned int y) const;

    bool m_active = false;
    Rect m_area;
    Image m_source;
    Image m_composite;
    std::unique_ptr<Texture> m_texture;
    std::vector<const Image*> m_masks;
    std::vector<const Texture*> m_ring;
    unsigned int m_maskWidth = 0;
    unsigned int m_maskHeight = 0;
    int m_timer = 0;
    int m_cutFrame = -1;
    int m_uploadedFrame = -1;
};

} // namespace gdl::game
