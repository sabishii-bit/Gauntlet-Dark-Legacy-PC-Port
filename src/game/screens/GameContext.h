#pragma once

#include <filesystem>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"

#include "game/config/GameConfig.h"
#include "game/world/TowerWorld.h"

namespace gdl::game {

/** What every screen needs from the game: settings, text, sound, the game's own files and
 * the unpacked data. */
struct GameContext {
    const GameConfig* config = nullptr;
    const StringTable* strings = nullptr;
    SoundPlayer* sounds = nullptr;       ///< optional; screens run silently without one
    const AssetLocator* assets = nullptr; ///< the game's files as shipped, for its streams
    TowerWorld* tower = nullptr;   ///< the hub level, loaded once and shared by the screens
    std::filesystem::path unpackedRoot;
};

} // namespace gdl::game
