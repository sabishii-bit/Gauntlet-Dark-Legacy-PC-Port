#pragma once

#include <string>
#include <string_view>

#include "engine/render/Mesh.h"

namespace gdl::formats {

/**
 * Encodes a mesh as a Wavefront OBJ: one object, one group per part named after its texture
 * index (`usemtl tex<index>`, plus `_lm<index>` when the part has a lightmap), texture
 * coordinates flipped to the OBJ bottom-left origin. A lightmapped mesh also carries one `vl`
 * line per vertex, its lightmap coordinates, indexed like `vt`.
 */
std::string encodeObj(const Mesh& mesh, std::string_view name);

} // namespace gdl::formats
