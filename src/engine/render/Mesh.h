#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/math/Math.h"

namespace gdl {

struct MeshVertex {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec2 uv{0.0f, 0.0f};
    Vec2 lightmapUv{0.0f, 0.0f};  ///< into the part's lightmap, in its texels, when it has one
    Color color = Color::white(); ///< the lighting baked into the vertex, when the mesh is prelit

    bool operator==(const MeshVertex&) const = default;
};

/** The triangles of a mesh that share one texture (and lightmap). */
struct MeshPart {
    std::uint32_t texture = 0;          ///< index into the archive's texture set
    std::uint32_t lightmap = 0;         ///< index of the lightmap texture, 0 for none
    std::vector<std::uint32_t> indices; ///< three per triangle, into Mesh::vertices
};

/** Static triangle geometry with one vertex array and per-texture index ranges. */
struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<MeshPart> parts;
    bool prelit = false; ///< the vertices' colours are the lighting the level was built with

    std::size_t triangleCount() const {
        std::size_t count = 0;
        for (const MeshPart& part : parts) {
            count += part.indices.size() / 3;
        }
        return count;
    }
};

} // namespace gdl
