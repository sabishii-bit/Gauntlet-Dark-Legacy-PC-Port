#pragma once

#include <filesystem>
#include <functional>

#include "engine/assets/StringTable.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/menu/ControlPrompts.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

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
    f32* stopTimeTotal = nullptr; ///< optional application-owned timer total shared across visits
    std::filesystem::path unpackedRoot;
    /** Applies and persists a settings edit; false leaves the active configuration unchanged. */
    std::function<bool(const GameConfig&)> saveSettings;
    /** Live audio preview while dragging a slider; persistence happens on release. */
    std::function<void(const AudioConfig&)> previewAudio;
    std::function<DisplayOptions()> displayOptions;
    /** Applies a temporary Video trial without writing settings. */
    std::function<bool(const GameConfig&)> previewVideo;
    std::function<void(s32, s32, ControlFeedback)> vibrate; ///< input player id, frames and cue
    std::function<void()> stopVibration;
    /** A completed secret challenge grants shared class availability, even before saving. */
    std::function<void(u16)> unlockClasses;
};

} // namespace gdl::game
