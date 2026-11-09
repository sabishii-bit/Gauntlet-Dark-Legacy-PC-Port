#pragma once

#include <span>

#include "formats/CritterWad.h"

namespace gdl::formats {

/** Decode the little-endian Dreamcast CRITTERS WAD into the common combat schema.
 * Native runtime pointers/caches are never imported. Throws FormatError on invalid data. */
CritterFile decodeLegendsDreamcastCritter(std::span<const u8> bytes);

} // namespace gdl::formats
