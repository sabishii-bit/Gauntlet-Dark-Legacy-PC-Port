#include "engine/ui/Canvas.h"

#include "engine/core/Assert.h"

namespace gdl {

namespace {

/** Every sprite shares one depth; the depth test keeps later draws in front. */
constexpr float kSpriteDepth = 0.5f;

} // namespace

Mat4 makeVirtualScreenTransform(const Mat4& frameProjection, float virtualWidth,
                                float virtualHeight, float frameWidth, float frameHeight) {
    return glm::scale(frameProjection,
                      Vec3{frameWidth / virtualWidth, frameHeight / virtualHeight, 1.0f});
}

void Canvas::begin(RenderDevice& device, const Mat4& transform) {
    GDL_VERIFY(m_device == nullptr, "Canvas::begin called twice");
    m_device = &device;
    m_transform = transform;
    m_texture = nullptr;
    m_batch.clear();
}

void Canvas::draw(const Texture& texture, const Rect& area, const Rect& uv, Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::draw outside begin/end");
    if (m_texture != nullptr && m_texture != &texture) {
        flush();
    }
    m_texture = &texture;
    m_batch.rect(area, kSpriteDepth, color, uv);
}

void Canvas::draw(const Texture& texture, const Rect& area, Color color) {
    draw(texture, area, Rect{0.0f, 0.0f, 1.0f, 1.0f}, color);
}

void Canvas::fill(const Rect& area, Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::fill outside begin/end");
    draw(m_device->whiteTexture(), area, color);
}

void Canvas::submit(const ImmediateBatch& batch, const Texture& texture, const Mat4& local) {
    GDL_VERIFY(m_device != nullptr, "Canvas::submit outside begin/end");
    flush();
    if (!batch.empty()) {
        m_device->draw(batch, texture, m_transform * local);
    }
}

void Canvas::end() {
    GDL_VERIFY(m_device != nullptr, "Canvas::end without begin");
    flush();
    m_device = nullptr;
    m_texture = nullptr;
}

void Canvas::flush() {
    if (!m_batch.empty() && m_texture != nullptr) {
        m_device->draw(m_batch, *m_texture, m_transform);
    }
    m_batch.clear();
}

} // namespace gdl
