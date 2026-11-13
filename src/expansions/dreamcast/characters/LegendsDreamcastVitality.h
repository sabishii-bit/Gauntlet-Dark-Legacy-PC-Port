#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/** The most health a character holds in the Dreamcast game: its status update puts anything over
 * it back every frame (8C0C16E0; the 9999.0 at 8C0C186C, stored at 8C0C1726). */
inline constexpr s32 kLegendsDreamcastHealthLimit = 9999;

} // namespace gdl::game
