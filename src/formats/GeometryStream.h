#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/render/Mesh.h"

namespace gdl::formats {

/**
 * Decodes one sub-object's packed vertex stream (the bytes ModelArchive keeps per sub-object)
 * into triangles, appended to `mesh` as one part using `texture`. Positions are fixed point
 * with 128 units per world unit, normals are packed 5-bit components, and texture coordinates
 * are fixed point with 128 units per texture width. Throws FormatError on a malformed stream.
 */
void decodeGeometryStream(std::span<const u8> stream, u32 texture, Mesh& mesh);

} // namespace gdl::formats
