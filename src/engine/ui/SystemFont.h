#pragma once

#include <string_view>

#include "engine/render/Image.h"

namespace gdl {

/** Rasterizes an ASCII line with an installed sans-serif font (Segoe UI on Windows,
 * DejaVu/Liberation/Noto Sans on Linux). White RGB, antialiased coverage in alpha.
 * Reads only OS fonts, not game assets; throws if no supported font is installed. */
Image rasterizeSystemText(std::string_view text, u32 pixelHeight);

} // namespace gdl
