#pragma once

#include <span>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** A special pickup the original stands in a level from its own code, not from the level's
 * file: a row of fNewPUPDescs (0x8012028C), its figure and flag from gOverlayPUPParams
 * (0x80120274). InitWorld's last call, AppendBigapePowerupsToScene (0x8007EC24), stands each
 * row whose level is the one entered. */
struct CodePlacedSpecial {
    std::string_view level; ///< the code of the level it stands in, such as D3
    std::string_view name;  ///< its figure in POWERUPS
    u32 flag = 0;           ///< the special it gives
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** The original's 39 rows in its order: 15 Mikeys, 13 Health Vampires and 11 Hands of Death
 * in 25 levels. */
std::span<const CodePlacedSpecial> codePlacedSpecials();

/** InLevel (0x800579E0): whether the level named `level` is `code`. The level's first four
 * letters must begin the code, and a level with no name is none. */
bool inLevel(std::string_view level, std::string_view code);

} // namespace gdl::game
