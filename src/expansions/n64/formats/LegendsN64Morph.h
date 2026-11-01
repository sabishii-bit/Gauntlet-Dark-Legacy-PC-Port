#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Mesh.h"

namespace gdl::formats {

inline constexpr usize kLegendsN64VariantByteLimit = usize{64} * 1024 * 1024;

/** Overflow-safe storage charge for mesh copies and their source-vertex identity.
 * Includes mesh/part records and vertex/index arrays; throws before cloning if
 * the shared archive budget is insufficient. Does not consume the budget. */
usize legendsN64VariantGeometryBytes(const Mesh& mesh, usize sourceVertices, usize frames,
                                     usize remainingBytes);

/** Native property6's absolute or accumulated delta vertex keys. `binding` begins at the BODY
 * control node's BDAT pointer: vertex count followed by one key index per native Vtx.
 * `vertexSources` maps decoded vertices back to that original Vtx pool, retaining
 * duplicated cache uploads. Deltas replay from the original signed16 positions,
 * not the preceding rendered pose. Malformed supported tracks throw FormatError. */
std::optional<std::vector<Mesh>>
decodeLegendsN64Morph(std::span<const u8> track, std::span<const u8> binding, const Mesh& original,
                      std::span<const u32> vertexSources, usize& remainingVertices,
                      usize& remainingBytes);

/** Property16 uses the same vertex writer but adds a timing/flag header. This
 * decoder accepts absolute or accumulated delta keys starting immediately;
 * delayed visibility returns nullopt. Animation direction belongs to the sequence. */
std::optional<std::vector<Mesh>>
decodeLegendsN64VertexKeys(std::span<const u8> track, std::span<const u8> binding,
                           const Mesh& original, std::span<const u32> vertexSources,
                           usize& remainingVertices, usize& remainingBytes);

} // namespace gdl::formats
