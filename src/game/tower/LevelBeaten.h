#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The level a party comes back to the tower having just beaten: the original's last level
 * beaten, which towerRecordLevelBeaten (0x800A1D30) sets as the level ends, the realm at
 * 0x803448AC and the level at 0x803448A8, and game_main clears as the party leaves the tower
 * again (0x80054A50). It is never saved; the tower is told it once, as the party arrives.
 */
struct LevelBeaten {
    s32 realm = -1; ///< the original's world id
    s32 level = -1; ///< its place in the realm's order, from nought

    bool operator==(const LevelBeaten&) const = default;
};

} // namespace gdl::game
