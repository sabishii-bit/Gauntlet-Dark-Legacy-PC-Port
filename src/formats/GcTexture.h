#pragma once

#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"

#include "formats/ModelArchive.h"

namespace gdl::formats {

/** Expands a GameCube RGB5A3 texel (native byte order already applied). */
Color rgb5a3ToColor(u16 texel);

/** Bits per pixel of a bitmap format; 0 for formats the GameCube build never uses. */
u32 bitsPerPixel(u8 format);

/** Bytes between the start of a bitmap's data and its first pixel (the palette, if any). */
usize pixelDataOffset(u8 format);

/** Bytes a bitmap occupies in the textures file: palette plus every mip level. */
usize bitmapDataSize(const ArchiveBitmap& bitmap);

/**
 * Decodes the top mip level of a bitmap from the textures.ngc contents into RGBA8.
 * Handles the tiled GameCube layouts, the RGB5A3 palettes, and the intensity formats,
 * which become white with the intensity as alpha, as the game's built-in palettes do.
 */
Image decodeGcTexture(const ArchiveBitmap& bitmap, std::span<const u8> texturesFile);

} // namespace gdl::formats
