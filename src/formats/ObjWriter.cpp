#include "formats/ObjWriter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>

namespace gdl::formats {

std::string encodeObj(const Mesh& mesh, std::string_view name) {
    std::string out;
    out += std::format("o {}\n", name);
    // A prelit mesh keeps its baked lighting as the vertex colour extension, 0 to 1.
    for (const MeshVertex& v : mesh.vertices) {
        if (mesh.prelit) {
            out += std::format("v {:.6g} {:.6g} {:.6g} {:.4g} {:.4g} {:.4g}\n", v.position.x,
                               v.position.y, v.position.z, static_cast<float>(v.color.r) / 255.0f,
                               static_cast<float>(v.color.g) / 255.0f,
                               static_cast<float>(v.color.b) / 255.0f);
        } else {
            out +=
                std::format("v {:.6g} {:.6g} {:.6g}\n", v.position.x, v.position.y, v.position.z);
        }
    }
    for (const MeshVertex& v : mesh.vertices) {
        out += std::format("vt {:.6g} {:.6g}\n", v.uv.x, 1.0f - v.uv.y);
    }
    for (const MeshVertex& v : mesh.vertices) {
        out += std::format("vn {:.6g} {:.6g} {:.6g}\n", v.normal.x, v.normal.y, v.normal.z);
    }
    if (std::ranges::any_of(mesh.parts, [](const MeshPart& part) { return part.lightmap != 0; })) {
        for (const MeshVertex& v : mesh.vertices) {
            out += std::format("vl {:.6g} {:.6g}\n", v.lightmapUv.x, v.lightmapUv.y);
        }
    }
    for (std::size_t p = 0; p < mesh.parts.size(); ++p) {
        const MeshPart& part = mesh.parts[p];
        out += std::format("g part{}\nusemtl tex{}", p, part.texture);
        if (part.lightmap != 0) {
            out += std::format("_lm{}", part.lightmap);
        }
        out += "\n";
        for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
            const std::uint32_t a = part.indices[i] + 1;
            const std::uint32_t b = part.indices[i + 1] + 1;
            const std::uint32_t c = part.indices[i + 2] + 1;
            out += std::format("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\n", a, b, c);
        }
    }
    return out;
}

} // namespace gdl::formats
