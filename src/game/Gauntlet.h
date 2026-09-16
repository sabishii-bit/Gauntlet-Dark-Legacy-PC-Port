#pragma once

#include <memory>
#include <string_view>

#include "engine/app/Application.h"
#include "engine/audio/AudioDevice.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/render/RenderDevice.h"

#include "game/AttractSequencer.h"
#include "game/CommandLine.h"
#include "game/MovieScene.h"
#include "game/SmokeTestScene.h"

namespace gdl::game {

/** The game: owns the top-level game flow and drives the engine each frame. */
class Gauntlet final : public Application {
public:
    Gauntlet(ApplicationDesc desc, GameOptions options);

    /** Size of the virtual frame the game draws into; it is letterboxed onto the window. */
    static constexpr f32 kFrameWidth = 640.0f;
    static constexpr f32 kFrameHeight = 448.0f;

protected:
    void onInit() override;
    void onUpdate(f64 deltaSeconds) override;
    void onRender(RenderDevice& device) override;
    void onShutdown() override;

private:
    bool startMovie(std::string_view name);
    void startNextAttractScreen();
    bool skipRequested() const;

    GameOptions m_options;
    std::unique_ptr<AudioDevice> m_audio;
    std::unique_ptr<AssetLocator> m_assets;
    AttractSequencer m_attract;
    MovieScene m_movie;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    f64 m_fpsAccumulator = 0.0;
    u32 m_fpsFrames = 0;
};

} // namespace gdl::game
