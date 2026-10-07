#include "engine/ui/Canvas.h"

#include <algorithm>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

/** Every sprite shares one depth; the depth test keeps later draws in front. */
constexpr f32 kSpriteDepth = 0.5f;

} // namespace

Mat4 makeVirtualScreenTransform(const Mat4& frameProjection, f32 virtualWidth, f32 virtualHeight,
                                f32 frameWidth, f32 frameHeight) {
    return glm::scale(frameProjection,
                      Vec3{frameWidth / virtualWidth, frameHeight / virtualHeight, 1.0f});
}

void Canvas::begin(RenderDevice& device, const Mat4& transform, const DrawState& state) {
    GDL_VERIFY(m_device == nullptr, "Canvas::begin called twice");
    m_device = &device;
    m_transform = transform;
    m_state = state;
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

void Canvas::fillScreen(Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::fillScreen outside begin/end");
    flush();
    ImmediateBatch batch;
    batch.rect({-1, -1, 2, 2}, kSpriteDepth, color);
    m_device->draw(batch, m_device->whiteTexture(), Mat4{1},
                   {.depthWrite = false, .depthTest = false});
}

void Canvas::fillTriangle(const Vec2& a, const Vec2& b, const Vec2& c, Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::fillTriangle outside begin/end");
    flush();
    ImmediateBatch batch;
    batch.begin(PrimitiveTopology::TriangleList);
    batch.vertex(Vec3{a, kSpriteDepth}, color, Vec2{0});
    batch.vertex(Vec3{b, kSpriteDepth}, color, Vec2{0});
    batch.vertex(Vec3{c, kSpriteDepth}, color, Vec2{0});
    batch.end();
    m_device->draw(batch, m_device->whiteTexture(), m_transform,
                   {.depthWrite = false, .depthTest = false});
}

void Canvas::fillHorizontalBand(f32 y, f32 height, Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::fillHorizontalBand outside begin/end");
    flush();
    const Vec4 a = m_transform * Vec4{0, y, kSpriteDepth, 1};
    const Vec4 b = m_transform * Vec4{0, y + height, kSpriteDepth, 1};
    const f32 top = std::clamp(std::min(a.y / a.w, b.y / b.w), -1.0f, 1.0f);
    const f32 bottom = std::clamp(std::max(a.y / a.w, b.y / b.w), -1.0f, 1.0f);
    if (bottom <= top) {
        return;
    }
    ImmediateBatch batch;
    batch.rect({-1, top, 2, bottom - top}, kSpriteDepth, color);
    m_device->draw(batch, m_device->whiteTexture(), Mat4{1},
                   {.depthWrite = false, .depthTest = false});
}

void Canvas::maskOutside(const Rect& area, Color color) {
    GDL_VERIFY(m_device != nullptr, "Canvas::maskOutside outside begin/end");
    flush();
    const Vec4 a = m_transform * Vec4{area.x, area.y, kSpriteDepth, 1};
    const Vec4 b = m_transform * Vec4{area.x + area.width, area.y + area.height, kSpriteDepth, 1};
    const f32 left = std::clamp(std::min(a.x / a.w, b.x / b.w), -1.0f, 1.0f);
    const f32 right = std::clamp(std::max(a.x / a.w, b.x / b.w), -1.0f, 1.0f);
    const f32 top = std::clamp(std::min(a.y / a.w, b.y / b.w), -1.0f, 1.0f);
    const f32 bottom = std::clamp(std::max(a.y / a.w, b.y / b.w), -1.0f, 1.0f);
    ImmediateBatch batch;
    const auto fillMargin = [&](const Rect& margin) {
        constexpr f32 kClipEpsilon = 1e-6f;
        if (margin.width > kClipEpsilon && margin.height > kClipEpsilon) {
            batch.rect(margin, kSpriteDepth, color);
        }
    };
    fillMargin({-1, -1, left + 1, 2});
    fillMargin({right, -1, 1 - right, 2});
    fillMargin({left, -1, right - left, top + 1});
    fillMargin({left, bottom, right - left, 1 - bottom});
    if (!batch.empty()) {
        m_device->draw(batch, m_device->whiteTexture(), Mat4{1},
                       {.depthWrite = false, .depthTest = false});
    }
}

void Canvas::submit(const ImmediateBatch& batch, const Texture& texture, const Mat4& local,
                    const DrawState& state) {
    GDL_VERIFY(m_device != nullptr, "Canvas::submit outside begin/end");
    flush();
    if (!batch.empty()) {
        m_device->draw(batch, texture, m_transform * local, state);
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
        m_device->draw(m_batch, *m_texture, m_transform, m_state);
    }
    m_batch.clear();
}

} // namespace gdl
