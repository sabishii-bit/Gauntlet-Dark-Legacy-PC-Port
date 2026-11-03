#pragma once

#include "engine/platform/DisplayMode.h"
#include "engine/render/RenderTypes.h"

#include "game/config/GameConfig.h"

namespace gdl::game {
/** The running game's settings: applied and saved, previewed, and the display it offers. */
class LiveSettings {
public:
    virtual ~LiveSettings() = default;
    /** Applies and persists `next`; false leaves the active configuration unchanged. */
    virtual bool save(const GameConfig& next) = 0;
    /** Applies a temporary Video trial without writing settings. */
    virtual bool previewVideo(const GameConfig& trial) = 0;
    /** Plays the audio as a slider moves; it is saved when the slider is let go. */
    virtual void previewAudio(const AudioConfig& audio) = 0;
    /** The displays, modes and window the Video page offers, as they stand now. */
    virtual DisplayOptions display() const = 0;
    /** The graphics API drawing now. */
    virtual GraphicsApi renderer() const = 0;
};
} // namespace gdl::game
