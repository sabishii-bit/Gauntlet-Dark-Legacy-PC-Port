#include "formats/ObjWriter.h"

#include <format>

namespace gdl::formats {

std::string encodeObj(const Mesh& mesh, std::string_view name) {
    std::string out;
    out += std::format("o {}\n", name);
    for (const MeshVertex& v : mesh.vertices) {
        out += std::format("v {:.6g} {:.6g} {:.6g}\n", v.position.x, v.position.y, v.position.z);
    }
    for (const MeshVertex& v : mesh.vertices) {
        out += std::format("vt {:.6g} {:.6g}\n", v.uv.x, 1.0f - v.uv.y);
    }
    for (const MeshVertex& v : mesh.vertices) {
        out += std::format("vn {:.6g} {:.6g} {:.6g}\n", v.normal.x, v.normal.y, v.normal.z);
    }
    for (usize p = 0; p < mesh.parts.size(); ++p) {
        const MeshPart& part = mesh.parts[p];
        out += std::format("g part{}\nusemtl tex{}\n", p, part.texture);
        for (usize i = 0; i + 2 < part.indices.size(); i += 3) {
            const u32 a = part.indices[i] + 1;
            const u32 b = part.indices[i + 1] + 1;
            const u32 c = part.indices[i + 2] + 1;
            out += std::format("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\n", a, b, c);
        }
    }
    return out;
}

} // namespace gdl::formats
