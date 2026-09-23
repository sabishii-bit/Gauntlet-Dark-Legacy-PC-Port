#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/Image.h"

namespace gdl::formats {

/** `image` enlarged `factor` times with each pixel repeated, so pixel art stays crisp. */
Image enlargeImage(const Image& image, std::uint32_t factor);

/**
 * Encodes images as a Windows icon (.ico): one 32-bit BGRA bitmap per image, with its alpha
 * channel and the classic 1-bit transparency mask. Throws FormatError for an image that is
 * empty or wider or taller than 256 pixels.
 */
std::vector<std::uint8_t> encodeIco(std::span<const Image> images);

} // namespace gdl::formats
