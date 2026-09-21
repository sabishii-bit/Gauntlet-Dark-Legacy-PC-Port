#pragma once

#include <filesystem>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"

#include "game/config/GameConfig.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** What every screen needs from the game: settings, text, sound, the game's own files and
 * the unpacked data. */
struct GameContext {
    const GameConfig* config = nullptr;
    const StringTable* strings = nullptr;
    SoundPlayer* sounds = nullptr;       ///< optional; screens run silently without one
    const AssetLocator* assets = nullptr; ///< the game's files as shipped, for its streams
    LevelWorld* tower = nullptr;   ///< the level in play, shared by the screens: the hub
                                   ///< tower until the party travels
    const LevelCatalog* levels = nullptr; ///< where exit portals lead; without it they are dead
    std::filesystem::path unpackedRoot;
};

} // namespace gdl::game
