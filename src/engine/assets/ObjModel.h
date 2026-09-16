#pragma once

#include <filesystem>
#include <string_view>

#include "engine/render/Mesh.h"

namespace gdl {

/**
 * Parses a Wavefront OBJ (positions, texture coordinates, normals, faces, `usemtl tex<N>`
 * groups) into a mesh; texture coordinates are converted to a top-left origin. Faces with more
 * than three corners become fans. Throws FormatError on malformed input.
 */
Mesh parseObj(std::string_view text);

/** Reads and parses an OBJ file; throws FileError or FormatError. */
Mesh loadObj(const std::filesystem::path& path);

} // namespace gdl
