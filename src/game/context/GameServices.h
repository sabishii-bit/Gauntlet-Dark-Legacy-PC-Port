#pragma once

#include <filesystem>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"

#include "game/config/GameConfig.h"
#include "game/hud/ControlPrompts.h"
#include "game/level/LevelCatalog.h"
#include "game/mechanics/MechanicCatalog.h"

namespace gdl::game {

/** What every screen reads from the running game: settings, text, the controls' labels, sound,
 * the game's own files and the unpacked data. The game owns one view, and a screen keeps it
 * from opening, so it must outlive every screen opened with it. */
struct GameServices {
    const GameConfig* config = nullptr;
    const StringTable* strings = nullptr;
    const ControlLabels* controlLabels = nullptr; ///< without them, the default bindings' names
    SoundPlayer* sounds = nullptr;                ///< optional; screens run silently without one
    AudioMixer* movieMixer = nullptr; ///< optional movie audio, independent of effects/music gain
    const AssetLocator* assets = nullptr; ///< the game's files as shipped, for its streams
    const LevelCatalog* levels = nullptr; ///< where exit portals lead; without it they are dead
    const MechanicCatalog* mechanics = nullptr; ///< what the build lays over play; none without
    std::filesystem::path unpackedRoot;

    /** The view a screen reads before it is first opened: nothing set. */
    static const GameServices& none() {
        static const GameServices kNone;
        return kNone;
    }
};

} // namespace gdl::game
