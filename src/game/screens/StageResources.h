#pragma once

#include "game/world/LevelWorld.h"

namespace gdl::game {
class Enemies;
class Critters;
class Bosses;
/** Native level dependencies shared by host loading and the presentation-only
 * client. These functions load artwork, never create actors or advance gameplay.
 * All catalogs must be frozen after preloading and before acknowledging loading. */
class StageResources {
public:
    static bool preload(const LevelWorld& world, Enemies& enemies, Critters& critters,
                        Bosses& bosses);
    /** Stable archive order includes future container Deaths and boss generators,
     * not only fixtures/actors visible at the entrance. Owners remain borrowed. */
    static std::vector<ItemArchive*> fixtureArchives(LevelWorld& world, Enemies& enemies,
                                                     Critters& critters, Bosses& bosses);
};
} // namespace gdl::game
