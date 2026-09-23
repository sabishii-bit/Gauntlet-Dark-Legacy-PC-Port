#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/render/Mesh.h"

namespace gdl::formats {

/**
 * Decodes one sub-object's packed vertex stream (the bytes ModelArchive keeps per sub-object)
 * into triangles, appended to `mesh` as one part using `texture` and, when not 0, the
 * `lightmap` texture. Positions are fixed point with 128 units per world unit, normals are
 * packed 5-bit components, and texture coordinates are fixed point with 128 units per texture
 * width; the four-value coordinate format carries the lightmap's pair as well, in texels of
 * the lightmap. Throws FormatError on a malformed stream.
 */
void decodeGeometryStream(std::span<const u8> stream, u32 texture, u32 lightmap, Mesh& mesh);
void decodeGeometryStream(std::span<const u8> stream, u32 texture, Mesh& mesh);

} // namespace gdl::formats
