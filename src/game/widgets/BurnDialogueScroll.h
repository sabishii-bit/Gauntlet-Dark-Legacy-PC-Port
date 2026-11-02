#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/**
 * Burns a menu backdrop away. Each mask frame is what is left of the scroll: its transparent
 * texels are burnt out, the hole grows frame by frame, and a ring of flame (the colour frames)
 * is drawn over the front. Runs on the original's tick clock: one frame every two ticks.
 */
class BurnDialogueScroll {
    struct Artwork {
        Image source;
        std::vector<const Image*> masks;
        std::vector<const Texture*> ring;
        NativeStateDigest::Digest sourceIdentity{};
    };
    struct Simulation {
        std::shared_ptr<const Artwork> art;
        Rect area;
        s32 timer = 0;
        bool active = false;
    };

public:
    class LocalSnapshot {
        friend class BurnDialogueScroll;
        Simulation m_state;
        const BurnDialogueScroll* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    static constexpr s32 kFrameCount = 21;
    static constexpr s32 kTicksPerFrame = 2;

    /** Read-only native-load preflight; performs no upload or animation step.
     * Short nonempty frame lists are valid: native playback repeats their last
     * entry over the same fixed hold. Does not guarantee future device allocation. */
    static bool ready(const Image* scroll, std::span<const Image* const> masks,
                      std::span<const Texture* const> ring);

    /**
     * Starts burning `scroll` over `area`. `masks` are the burn frames (alpha zero is burnt),
     * stretched over the scroll with filtering; `ring` the flame frames drawn on top. Retains
     * immutable source pixels; the GPU copy is prepared only during rendering.
     */
    bool start(RenderDevice& device, const Rect& area, const Image& scroll,
               std::vector<const Image*> masks, std::vector<const Texture*> ring);

    void reset();
    bool active() const { return m_sim.active; }
    s32 frame() const { return m_sim.timer / kTicksPerFrame; }

    /** Advances the effect; it ends once the last frame has played. */
    void step(s32 ticks);

    /** Uploads the current frame's cut-out scroll; call after beginFrame and before drawing. */
    void prepare(RenderDevice& device);

    void draw(Canvas& canvas) const;
    /** The same filtered mask composition, usable for preloaded presentation frames. */
    static Image composite(const Image& scroll, const Image& mask);

    /** Alpha of the composited scroll at a texel, for tests. */
    u8 compositeAlpha(u32 x, u32 y) const;

private:
    static bool appendSimulationDigest(NativeStateDigest& digest, const Simulation& state);
    static f32 maskAlpha(const Image& source, const Image& mask, u32 x, u32 y);

    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    // Derived presentation cache: never copied or uploaded during simulation rewind.
    std::unique_ptr<Texture> m_texture;
    std::shared_ptr<const Artwork> m_uploadedArt;
    const RenderDevice* m_device = nullptr;
    s32 m_uploadedFrame = -1;
};

} // namespace gdl::game
