#pragma once

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
    void begin(RenderDevice& device, const Mat4& transform);

    void draw(const Texture& texture, const Rect& area, const Rect& uv, Color color);
    void draw(const Texture& texture, const Rect& area, Color color = Color::white());
    void fill(const Rect& area, Color color);

    /** Draws a prepared batch with `local` applied before the canvas transform, after any
     * pending sprites. */
    void submit(const ImmediateBatch& batch, const Texture& texture, const Mat4& local);

    /** Submits what is pending; must be called before the frame ends. */
    void end();

    bool active() const { return m_device != nullptr; }

private:
    void flush();

    RenderDevice* m_device = nullptr;
    Mat4 m_transform{1.0f};
    const Texture* m_texture = nullptr;
    ImmediateBatch m_batch;
};

} // namespace gdl
