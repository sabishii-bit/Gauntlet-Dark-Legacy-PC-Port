#pragma once

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl {

/** Maps a virtual pixel space onto a frame that `frameProjection` already maps to clip space. */
Mat4 makeVirtualScreenTransform(const Mat4& frameProjection, f32 virtualWidth, f32 virtualHeight,
                                f32 frameWidth, f32 frameHeight);

/** Textured rectangles in a virtual pixel space, drawn in call order and batched by texture. */
class Canvas {
public:
    void begin(RenderDevice& device, const Mat4& transform, const DrawState& state = {});

    void draw(const Texture& texture, const Rect& area, const Rect& uv, Color color);
    void draw(const Texture& texture, const Rect& area, Color color = Color::white());
    void fill(const Rect& area, Color color);
    void fillTriangle(const Vec2& a, const Vec2& b, const Vec2& c, Color color);
    /** Covers the entire viewport, including margins outside the virtual canvas. */
    void fillScreen(Color color);
    /** Fills a virtual-height band across the entire viewport width. */
    void fillHorizontalBand(f32 y, f32 height, Color color);
    /** Masks the viewport outside an axis-aligned virtual-space rectangle. */
    void maskOutside(const Rect& area, Color color = Color::black());
    /** Masks the viewport's whole width above virtual row `top` and below row `bottom`, out
     * to its edges, leaving the rows between alone and whatever lies beside them. */
    void maskAboveAndBelow(f32 top, f32 bottom, Color color = Color::black());

    /** Draws a prepared batch with `local` applied before the canvas's local transform and its
     * own, after any pending sprites. */
    void submit(const ImmediateBatch& batch, const Texture& texture, const Mat4& local,
                const DrawState& state = {});

    /** Draws what follows through `local` before the canvas's own transform (a game's native
     * frame); what is pending is flushed first. begin() and end() reset it. */
    void setLocalTransform(const Mat4& local);
    const Mat4& localTransform() const { return m_local; }

    /** Submits what is pending; must be called before the frame ends. */
    void end();

    bool active() const { return m_device != nullptr; }

private:
    void flush();
    /** What a draw is placed by: the local transform, then the canvas's own; the canvas's own
     * alone while there is no local one, since a product with the identity loses -0.0. */
    Mat4 placement() const;

    RenderDevice* m_device = nullptr;
    Mat4 m_transform{1.0f};
    Mat4 m_local{1.0f};
    bool m_hasLocal = false; ///< set while a local transform other than the identity applies
    DrawState m_state;
    const Texture* m_texture = nullptr;
    ImmediateBatch m_batch;
};

/** Composes `local` onto a canvas's local transform until it goes out of scope. */
class CanvasFrame {
public:
    CanvasFrame(Canvas& canvas, const Mat4& local);
    /** Restores the local transform the frame found. */
    ~CanvasFrame();
    GDL_NON_COPYABLE_NON_MOVABLE(CanvasFrame);

private:
    Canvas& m_canvas;
    Mat4 m_previous;
};

} // namespace gdl
