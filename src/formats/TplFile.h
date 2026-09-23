#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/Image.h"

namespace gdl::formats {

/** GX texture formats a .tpl may hold that the reader decodes. */
inline constexpr std::uint32_t kTplRgb565 = 4;
inline constexpr std::uint32_t kTplRgb5a3 = 5;
inline constexpr std::uint32_t kTplRgba8 = 6;

/** One picture of a texture palette library, decoded to RGBA8. */
struct TplImage {
    std::uint32_t format = 0; ///< the GX format it was stored in
    Image image;
};

/**
 * Decodes every image of a GameCube texture palette library (.tpl), the container the
 * memory-card icon and banner use: big-endian headers and 4x4-tiled texels. Throws
 * FormatError on a malformed file or a format other than RGB565, RGB5A3 and RGBA8.
 */
std::vector<TplImage> parseTplFile(std::span<const std::uint8_t> bytes);

} // namespace gdl::formats
