#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/**
 * Unpacks one movie frame block. The block starts with a four-byte method word: 1 means the
 * rest is stored verbatim, anything else means LZSS with 16-bit flag words, 12-bit distances
 * and lengths of 3 to 18. Throws FormatError on malformed input.
 */
std::vector<u8> lzssUnpack(std::span<const u8> packed);

} // namespace gdl
