#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/math/Math.h"
#include "engine/render/Image.h"

#include "formats/ModelArchive.h"

namespace gdl::formats {

/** Expands a GameCube RGB5A3 texel (native byte order already applied). */
Color rgb5a3ToColor(std::uint16_t texel);

/** Bits per pixel of a bitmap format; 0 for formats the GameCube build never uses. */
std::uint32_t bitsPerPixel(std::uint8_t format);

/** Bytes between the start of a bitmap's data and its first pixel (the palette, if any). */
std::size_t pixelDataOffset(std::uint8_t format);

/** Bytes a bitmap occupies in the textures file: palette plus every mip level. */
std::size_t bitmapDataSize(const ArchiveBitmap& bitmap);

/**
 * Decodes the top mip level of a bitmap from the textures.ngc contents into RGBA8.
 * Handles the tiled GameCube layouts, the RGB5A3 palettes, and the intensity formats,
 * which become white with the intensity as alpha, as the game's built-in palettes do.
 */
Image decodeGcTexture(const ArchiveBitmap& bitmap, std::span<const std::uint8_t> texturesFile);

} // namespace gdl::formats
