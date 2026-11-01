#pragma once

#include <span>
#include <vector>

#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::formats {

/** Decode one native 12-byte lightmap descriptor (RGB565 colour, not alpha intensity). */
Image decodeDreamcastLightmap(std::span<const u8> models, std::span<const u8> pixels, u32 record);

/** Decode v1 Dreamcast bitmap slots and native textures.rom pixels.
 * Supports RGB565/ARGB1555/ARGB4444, twiddled/rectangular and normal/small VQ,
 * including the largest authored mip. Borrowed slots keep empty images.
 * Unsupported encodings and malformed buffers throw FormatError. */
std::vector<LegendsMapTexture> decodeLegendsDreamcastTextures(std::span<const u8> models,
                                                              std::span<const u8> pixels);

} // namespace gdl::formats
