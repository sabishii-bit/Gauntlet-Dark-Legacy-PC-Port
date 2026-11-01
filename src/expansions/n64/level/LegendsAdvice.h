#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/** Native e0070d2c resets advice on entering player selection, not on changing
 * maps. The app shares this transient state with its native levels; it is not
 * character progression and must never be written into a save. */
struct LegendsAdviceState {
    bool deathShown = false;

    bool admitDeath(s32 lowestLevel, s32 maximumLevel) {
        if (deathShown || lowestLevel < 1 || lowestLevel > maximumLevel) {
            return false;
        }
        deathShown = true;
        return true;
    }
};

} // namespace gdl::game
