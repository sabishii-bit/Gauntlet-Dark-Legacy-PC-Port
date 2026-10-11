#pragma once
#include <array>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
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
#include "game/app/BuildLabel.h"
#include "game/app/CommandLine.h"
#include "game/app/LevelCompletion.h"
#include "game/app/MusicDuck.h"
#include "game/app/OnlineRun.h"
#include "game/config/GameConfig.h"
#include "game/players/CursorAim.h"
#include "game/players/PlayerControls.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/AttractScene.h"
#include "game/screens/GameContext.h"
#include "game/screens/IdleScreen.h"
#include "game/screens/LevelExitSpeech.h"
#include "game/screens/LevelLoadingScreen.h"
#include "game/screens/MovieScene.h"
#include "game/screens/PauseMenu.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerSelectScene.h"
#include "game/screens/SessionInputs.h"
#include "game/screens/SmokeTestScene.h"
#include "game/screens/TitleScene.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** The game: owns the top-level game flow and drives the engine each frame. */
class Gauntlet final : public Application {
public:
    Gauntlet(ApplicationDesc desc, GameOptions options, GameConfig config,
             std::string_view version);

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
    void updateAttract(f64 deltaSeconds);
    bool updateIdle(f64 deltaSeconds);
    void updateTitle(f64 deltaSeconds);
    void updateSelect(f64 deltaSeconds);
    void updateNetplay(f64 deltaSeconds);
    void connectNetplay();
    void leaveNetplay();
    void updateTower(f64 deltaSeconds);
    PlayScene::Inputs readPlayInputs(f64 deltaSeconds);
    void resetPlayInput();
    void updatePause(f64 deltaSeconds);
    bool saveSettings(const GameConfig& config);
    bool applySettings(const GameConfig& config, bool persist);
    void updateAfterLevel(f64 deltaSeconds);
    void finishJourney();
    void updateJourney(f64 deltaSeconds);
    bool updateCompletion(f64 deltaSeconds);
    /** Writes the party in play back into its save slots. */
    void keepParty();
    void keepParty(std::span<const PartyMember> party);
    bool startTower(std::span<const PartyMember> party, const PlayOptions& options = {});
    /** Loads `level` and brings the party into it. */
    bool startLevel(const LevelRef& level, std::span<const PartyMember> party,
                    const PlayOptions& options = {});
    bool startScenario(const std::filesystem::path& file);
    bool startPlayerSelect(s32 startingPlayer, std::span<const PartyMember> party = {},
                           bool manage = false);
    /** `player` joins the party in the tower by way of the select screen. */
    bool joinTower(const PlayerSelectScene::Inputs& joining);
    s32 playerPressingStart() const;
    GameContext context();
    void applyWindowIcon();
    void renderScene(RenderDevice& device);

    GameOptions m_options;
    GameConfig m_config;
    BuildLabel m_buildLabel;
    std::string m_version;
    std::unique_ptr<OnlineRun> m_online;
    struct NetplayNavigation {
        NetplayMenu menu;
        NetplayMenu::View view;
        NetplayMenu::Page previous = NetplayMenu::Page::Choose;
        bool overlay = true;
        s32 pauseOwner = 0;
        std::string pendingInvitation;
        std::string digest;
        std::string notice;
        std::future<std::string> digestResult;
        std::jthread digestWorker;
    };
    std::unique_ptr<NetplayNavigation> m_netplay;
    std::array<PlayerControlReader, PlayScene::kPlayerCount> m_controls;
    SessionInputs m_sessionInputs;
    std::array<CursorInput, PlayScene::kPlayerCount> m_cursorInput;
    PromptDevices m_promptDevices;
    StringTable m_strings;
    std::unique_ptr<AudioDevice> m_audio;
    std::unique_ptr<SoundPlayer> m_sounds;
    std::unique_ptr<AssetLocator> m_assets;
    AttractSequencer m_attract;
    AttractScene m_demo;
    IdleWatch m_idleWatch;
    IdleScreen m_idleScreen;
    MovieScene m_movie;
    TitleScene m_title;
    PlayerSelectScene m_select;
    LevelCatalog m_levels;
    f32 m_stopTimeTotal = 0; ///< most recent pickup's total, shared by parent and secret levels
    struct PlaySession {
        PlaySession() = default;
        PlaySession(const PlaySession&) = delete;
        PlaySession& operator=(const PlaySession&) = delete;
        PlaySession(PlaySession&&) = delete;
        PlaySession& operator=(PlaySession&&) = delete;
        LevelWorld world;
        PlayScene scene;
        ~PlaySession() { scene.close(); }
    };
    std::unique_ptr<PlaySession> m_play = std::make_unique<PlaySession>();
    std::unique_ptr<PlaySession> m_parent; ///< stage retained during a secret challenge
    PauseMenu m_pause;
    MusicDuck m_musicDuck; ///< the music under the pause menu
    AfterLevelScene m_afterLevel;
    LevelExitSpeech m_exitSpeech;
    /** A journey between levels: the picture is drawn over an empty view for a frame, so
     * that it is on screen while the next level loads, which holds everything up. */
    struct Journey {
        struct Completion {
            LevelCompletion flow;
            std::vector<LevelResults> results;
            std::array<s32, 3> maxima;
            std::string levelName;
        };
        std::optional<Completion> completion;
        LevelRef destination;
        std::vector<PartyMember> party;
        PlayOptions options;
        bool shown = false; ///< the covering frame has been drawn
        bool presentationStarted = false;
        bool movieStarted = false;
        std::string movie;
        bool secret = false;
    };
    std::optional<Journey> m_journey;
    void returnFromChallenge(std::span<const PartyMember> party);
    SaveSlots m_saves; ///< where the party in play is kept
    TransitionScreen m_loadingPicture;
    LevelLoadingScreen m_levelLoading;
    Canvas m_canvas;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    bool m_titleWarned = false;
    f64 m_fpsAccumulator = 0.0;
    u64 m_fpsLastFrame = 0;
};

} // namespace gdl::game
