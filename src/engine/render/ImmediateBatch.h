#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "engine/render/RenderTypes.h"

namespace gdl {

/**
 * CPU-side geometry builder. Accepts lists, strips, fans and quads and
 * accumulates one triangle list for RenderDevice::draw().
 */
class ImmediateBatch {
public:
    void clear();

    void begin(PrimitiveTopology topology);
    void vertex(const Vec3& position, Color color, const Vec2& uv);
    void vertex(const Vec3& position, Color color, const Vec2& uv, const Vec2& uv2);
    void vertex(const ImmediateVertex& v);
    void end();

    /** Adds an axis-aligned textured rectangle at a fixed depth. */
    void rect(const Rect& area, float depth, Color color,
              const Rect& uv = Rect{0.0f, 0.0f, 1.0f, 1.0f});

    bool empty() const { return m_triangles.empty(); }
    std::size_t vertexCount() const { return m_triangles.size(); }
    std::span<const ImmediateVertex> triangles() const { return m_triangles; }

private:
    void flushPrimitive();

    std::vector<ImmediateVertex> m_triangles;
    std::vector<ImmediateVertex> m_pending;
    PrimitiveTopology m_topology = PrimitiveTopology::TriangleList;
    bool m_open = false;
};

} // namespace gdl
