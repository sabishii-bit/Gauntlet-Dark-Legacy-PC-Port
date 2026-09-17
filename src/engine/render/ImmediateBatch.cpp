#include "engine/render/ImmediateBatch.h"

#include "engine/core/Assert.h"

namespace gdl {

void ImmediateBatch::clear() {
    m_triangles.clear();
    m_pending.clear();
    m_open = false;
}

void ImmediateBatch::begin(PrimitiveTopology topology) {
    GDL_ASSERT(!m_open, "ImmediateBatch::begin called twice without end");
    m_topology = topology;
    m_pending.clear();
    m_open = true;
}

void ImmediateBatch::vertex(const Vec3& position, Color color, const Vec2& uv) {
    vertex(ImmediateVertex{position, color, uv});
}

void ImmediateBatch::vertex(const Vec3& position, Color color, const Vec2& uv, const Vec2& uv2) {
    vertex(ImmediateVertex{position, color, uv, uv2});
}

void ImmediateBatch::vertex(const ImmediateVertex& v) {
    GDL_ASSERT(m_open, "ImmediateBatch::vertex called outside begin/end");
    m_pending.push_back(v);
}

void ImmediateBatch::end() {
    GDL_ASSERT(m_open, "ImmediateBatch::end called without begin");
    flushPrimitive();
    m_pending.clear();
    m_open = false;
}

void ImmediateBatch::rect(const Rect& area, f32 depth, Color color, const Rect& uv) {
    begin(PrimitiveTopology::QuadList);
    vertex(Vec3{area.x, area.y, depth}, color, Vec2{uv.x, uv.y});
    vertex(Vec3{area.right(), area.y, depth}, color, Vec2{uv.right(), uv.y});
    vertex(Vec3{area.right(), area.bottom(), depth}, color, Vec2{uv.right(), uv.bottom()});
    vertex(Vec3{area.x, area.bottom(), depth}, color, Vec2{uv.x, uv.bottom()});
    end();
}

void ImmediateBatch::flushPrimitive() {
    const auto& v = m_pending;
    const usize n = v.size();

    switch (m_topology) {
    case PrimitiveTopology::TriangleList:
        for (usize i = 0; i + 2 < n; i += 3) {
            m_triangles.push_back(v[i]);
            m_triangles.push_back(v[i + 1]);
            m_triangles.push_back(v[i + 2]);
        }
        break;

    case PrimitiveTopology::TriangleStrip:
        for (usize i = 2; i < n; ++i) {
            if ((i & 1U) == 0) {
                m_triangles.push_back(v[i - 2]);
                m_triangles.push_back(v[i - 1]);
            } else {
                m_triangles.push_back(v[i - 1]);
                m_triangles.push_back(v[i - 2]);
            }
            m_triangles.push_back(v[i]);
        }
        break;

    case PrimitiveTopology::TriangleFan:
        for (usize i = 2; i < n; ++i) {
            m_triangles.push_back(v[0]);
            m_triangles.push_back(v[i - 1]);
            m_triangles.push_back(v[i]);
        }
        break;

    case PrimitiveTopology::QuadList:
        for (usize i = 0; i + 3 < n; i += 4) {
            m_triangles.push_back(v[i]);
            m_triangles.push_back(v[i + 1]);
            m_triangles.push_back(v[i + 2]);
            m_triangles.push_back(v[i]);
            m_triangles.push_back(v[i + 2]);
            m_triangles.push_back(v[i + 3]);
        }
        break;
    }
}

} // namespace gdl
