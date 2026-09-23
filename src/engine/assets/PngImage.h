#pragma once

#include <filesystem>
#include <span>

#include "engine/core/Types.h"
#include "engine/render/Image.h"

namespace gdl {

/** Decodes a PNG (or any other common image format) into RGBA8; throws FormatError. */
Image decodeImageFile(std::span<const u8> bytes);

/** Reads and decodes an image file; throws FileError or FormatError. */
Image loadImageFile(const std::filesystem::path& path);

} // namespace gdl
