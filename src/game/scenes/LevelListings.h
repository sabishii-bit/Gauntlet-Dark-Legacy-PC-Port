#pragma once
#include <filesystem>
#include <functional>

#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"

namespace gdl::game {

class LevelCatalog;
class LevelWorld;
struct CharacterSave;

/** What the level's listings read of the game, each read as `list` runs. */
struct LevelListingsAccess {
    /** The world the console reads: the shared game's online, offline play's while it is open;
     * null with nothing in play. */
    std::function<const LevelWorld*()> viewed;
    const LevelCatalog& levels;
    const std::filesystem::path& unpackedRoot; ///< where a named map's layout is read from
    /** The save of the character the console speaks for; null with nobody in. */
    std::function<const CharacterSave*()> save;
};

/**
 * Registers `list` with `host`, over `listings`, and the listings of the levels with
 * `listings`, in the order its usage names them: maps, items, inventory, enemies, critters,
 * generators, objects and switches. A map named after a listing, in any case, is read from
 * the unpacked data instead of the level in play.
 */
void registerLevelListings(ConsoleHost& host, ConsoleListings& listings,
                           const LevelListingsAccess& access);

} // namespace gdl::game
