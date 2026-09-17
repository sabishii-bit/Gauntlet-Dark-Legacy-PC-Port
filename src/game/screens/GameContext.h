#pragma once

#include <filesystem>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"

#include "game/config/GameConfig.h"

namespace gdl::game {

/** What every screen needs from the game: settings, text, sound and the unpacked data. */
struct GameContext {
    const GameConfig* config = nullptr;
    const StringTable* strings = nullptr;
    SoundPlayer* sounds = nullptr; ///< optional; screens run silently without one
    std::filesystem::path unpackedRoot;
};

} // namespace gdl::game
