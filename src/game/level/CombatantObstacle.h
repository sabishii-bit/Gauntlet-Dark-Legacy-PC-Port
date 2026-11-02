#pragma once

#include "engine/core/Types.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {
/** Something of the level's where a great one walks (CritterCollideItems, fn_8005D5C8): it
 * stops the great one, but a golem or gargoyle walks through a chest and breaks what is
 * breakable, stopping only while it stands or as it blows up. */
struct CombatantObstacle {
    enum class Kind : u8 { Blocks, Chest, Breakable, LowGenerator };
    Obstacle box;
    Kind kind = Kind::Blocks;
    s32 id = -1; ///< a breakable's: a barrel's index, or a generator's from 2000
    s32 health = 0;
    s32 armor = 0;
    bool explodes = false;
};
} // namespace gdl::game
