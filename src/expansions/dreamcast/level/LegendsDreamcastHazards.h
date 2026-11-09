#pragma once

#include <vector>

#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::game {

/** Translate Dreamcast world/ancestor harm flags into the common surface-contact
 * policy. Owner indices remain the native collision roster, including meshless
 * parents. Invalid hierarchies are rejected, not partly admitted. */
std::vector<u32> dreamcastSurfaceHazardFlags(const formats::LegendsMap& map);

} // namespace gdl::game
