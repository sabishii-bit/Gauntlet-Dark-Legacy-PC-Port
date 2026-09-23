#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/app/Application.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioDevice.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "game/app/AttractSequencer.h"
#include "game/app/CommandLine.h"
#include "game/config/GameConfig.h"
#include "game/screens/GameContext.h"
#include "game/screens/MovieScene.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerSelectScene.h"
#include "game/screens/SmokeTestScene.h"
#include "game/screens/TitleScene.h"
#include "game/world/LevelWorld.h"

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
    void updateSelect(f64 deltaSeconds);
    void updateTower(f64 deltaSeconds);
    void finishJourney();
    /** Writes the party in play back into its save slots. */
    void keepParty();
    bool startTower(std::span<const PartyMember> party, const PlayOptions& options = {});
    /** Loads `level` and brings the party into it. */
    bool startLevel(const LevelRef& level, std::span<const PartyMember> party,
                    const PlayOptions& options = {});
    bool startScenario(const std::filesystem::path& file);
    bool startPlayerSelect(s32 startingPlayer);
    s32 playerPressingStart() const;
    GameContext context();
    void applyWindowIcon();

    GameOptions m_options;
    GameConfig m_config;
    StringTable m_strings;
    std::unique_ptr<AudioDevice> m_audio;
    std::unique_ptr<SoundPlayer> m_sounds;
    std::unique_ptr<AssetLocator> m_assets;
    AttractSequencer m_attract;
    MovieScene m_movie;
    TitleScene m_title;
    PlayerSelectScene m_select;
    LevelCatalog m_levels;
    LevelWorld m_towerWorld; ///< the level in play: the tower until the party travels
    PlayScene m_tower;
    /** A journey between levels: the picture is drawn over an empty view for a frame, so
     * that it is on screen while the next level loads, which holds everything up. */
    struct Journey {
        LevelRef destination;
        std::vector<PartyMember> party;
        PlayOptions options;
        bool shown = false; ///< the covering frame has been drawn
    };
    std::optional<Journey> m_journey;
    SaveSlots m_saves; ///< where the party in play is kept
    TransitionScreen m_loadingPicture;
    Canvas m_canvas;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    bool m_titleWarned = false;
    f64 m_fpsAccumulator = 0.0;
    u32 m_fpsFrames = 0;
};

} // namespace gdl::game
