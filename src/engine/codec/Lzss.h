#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gdl {

/**
 * Unpacks one movie frame block. The block starts with a four-byte method word: 1 means the
 * rest is stored verbatim, anything else means LZSS with 16-bit flag words, 12-bit distances
 * and lengths of 3 to 18. Throws FormatError on malformed input.
 */
std::vector<std::uint8_t> lzssUnpack(std::span<const std::uint8_t> packed);

} // namespace gdl
