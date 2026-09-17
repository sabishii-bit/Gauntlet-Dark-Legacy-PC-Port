#pragma once

#include <memory>
#include <string_view>

#include "engine/app/Application.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioDevice.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/render/RenderDevice.h"

#include "game/app/AttractSequencer.h"
#include "game/app/CommandLine.h"
#include "game/config/GameConfig.h"
#include "game/screens/GameContext.h"
#include "game/screens/MovieScene.h"
#include "game/screens/SmokeTestScene.h"
#include "game/screens/TitleScene.h"

namespace gdl::game {

/** The game: owns the top-level game flow and drives the engine each frame. */
class Gauntlet final : public Application {
public:
    Gauntlet(ApplicationDesc desc, GameOptions options, GameConfig config);

protected:
    void onInit() override;
    void onUpdate(f64 deltaSeconds) override;
    void onRender(RenderDevice& device) override;
    void onShutdown() override;

private:
    bool startMovie(std::string_view name);
    bool startTitleScreen();
    void startNextAttractScreen();
    void updateMovie(f64 deltaSeconds);
    void updateTitle(f64 deltaSeconds);
    GameContext context() const;

    GameOptions m_options;
    GameConfig m_config;
    StringTable m_strings;
    std::unique_ptr<AudioDevice> m_audio;
    std::unique_ptr<SoundPlayer> m_sounds;
    std::unique_ptr<AssetLocator> m_assets;
    AttractSequencer m_attract;
    MovieScene m_movie;
    TitleScene m_title;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    bool m_titleWarned = false;
    f64 m_fpsAccumulator = 0.0;
    u32 m_fpsFrames = 0;
};

} // namespace gdl::game
