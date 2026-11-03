#pragma once

#include <filesystem>
#include <functional>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/render/RenderTypes.h"

#include "game/config/GameConfig.h"
#include "game/hud/ControlPrompts.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/mechanics/MechanicCatalog.h"

namespace gdl::game {

/** What every screen needs from the game: settings, text, sound, the game's own files and
 * the unpacked data. */
struct GameContext {
    const GameConfig* config = nullptr;
    const StringTable* strings = nullptr;
    ControlLabels controlLabels;      ///< live bindings for a player; -1 denotes a shared prompt
    SoundPlayer* sounds = nullptr;    ///< optional; screens run silently without one
    AudioMixer* movieMixer = nullptr; ///< optional movie audio, independent of effects/music gain
    const AssetLocator* assets = nullptr; ///< the game's files as shipped, for its streams
    LevelWorld* tower = nullptr;          ///< the level in play, shared by the screens: the hub
                                          ///< tower until the party travels
    const LevelCatalog* levels = nullptr; ///< where exit portals lead; without it they are dead
    const MechanicCatalog* mechanics = nullptr; ///< what the build lays over play; none without
    f32* stopTimeTotal = nullptr; ///< optional application-owned timer total shared across visits
    std::filesystem::path unpackedRoot;
    /** Applies and persists a settings edit; false leaves the active configuration unchanged. */
    std::function<bool(const GameConfig&)> saveSettings;
    /** Live audio preview while dragging a slider; persistence happens on release. */
    std::function<void(const AudioConfig&)> previewAudio;
    std::function<DisplayOptions()> displayOptions;
    /** Applies a temporary Video trial without writing settings. */
    std::function<bool(const GameConfig&)> previewVideo;
    /** The graphics API drawing now; the Video page says a restart is needed for another. */
    GraphicsApi renderer = GraphicsApi::Vulkan;
};

} // namespace gdl::game
