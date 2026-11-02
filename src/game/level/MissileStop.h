#pragma once

#include "engine/core/Types.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {
/** Something of the level's in the way of the swarm's missiles besides the world
 * (fn_8005ED44, SfxSkipItem): doors, chests, barrels, generators, bottles lying about, the
 * triggers that are shot and raised tent walls stop one unharmed; a standing safe rock takes
 * its blow, and a piercing bolt stops only at a rock that blow leaves standing. */
struct MissileStop {
    Obstacle box;
    s32 rock = -1;      ///< the safe rock it is, or -1
    s32 rockHealth = 0; ///< that rock's health and armour
    s32 rockArmor = 0;

    static MissileStop of(const Obstacle& box) { return MissileStop{.box = box}; }
};
} // namespace gdl::game
