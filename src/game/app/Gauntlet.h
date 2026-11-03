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

#include "expansions/shared/scenes/LegendsMapCatalog.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "game/app/AttractSequencer.h"
#include "game/app/BuildLabel.h"
#include "game/app/CommandLine.h"
#include "game/app/LevelCompletion.h"
#include "game/app/LevelDebugOverlay.h"
#include "game/app/OfflineConsoleCommands.h"
#include "game/app/OnlineRun.h"
#include "game/audio/MusicDuck.h"
#include "game/camera/FreeCamera.h"
#include "game/config/GameConfig.h"
#include "game/console/DevConsole.h"
#include "game/console/NoclipMotion.h"
#include "game/context/GameContext.h"
#include "game/hud/AutoSaveIcon.h"
#include "game/level/LevelWorld.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/menu/PauseMenu.h"
#include "game/players/CursorAim.h"
#include "game/players/PlayerControls.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/AttractScene.h"
#include "game/scenes/IdleScreen.h"
#include "game/scenes/LevelLoadingScreen.h"
#include "game/scenes/MovieScene.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayerSelectScene.h"
#include "game/scenes/SceneSelection.h"
#include "game/scenes/SmokeTestScene.h"
#include "game/scenes/TitleScene.h"
#include "game/tower/LevelExitSpeech.h"

namespace gdl::game {

struct LegendsAdviceState;

/** The game: owns the top-level game flow and drives the engine each frame. */
class Gauntlet final : public Application {
public:
    /** `mechanics` are those the build carries, laid over every level. */
    Gauntlet(ApplicationDesc desc, GameOptions options, GameConfig config, std::string_view version,
             MechanicCatalog mechanics = {});

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
    /** The level debug overlay's lines for the level in play. */
    std::vector<std::string> levelDebugLines() const;
    /** The developer console's font, banner and commands (GauntletConsole.cpp). */
    void setUpConsole();
    const PlayScene* consoleScene() const;
    const LevelWorld* consoleWorld() const;
    /** What the offline console commands read and change of the game. */
    OfflineConsoleCommands::Ports offlineConsolePorts();
    bool dispatchOnlineConsole(DevConsole& console, std::string_view name,
                               DevConsole::Arguments arguments);
    bool updateOnlineConsole(f64 deltaSeconds, bool overlay = false);
    /** The player the console speaks for: the one at the keyboard, else the first in; -1
     * with nobody in. */
    s32 consolePlayer();
    void consoleGoto(DevConsole& console, DevConsole::Arguments arguments);
    /** The installed Legends maps of a platform, found as the catalogue finds them. */
    /** The Legends maps installed for a platform. The disk is walked on the first ask and
     * whenever `rescan` asks (list maps, goto); the console's completion reads what was
     * found, so typing costs nothing when there is no Legends content at all. */
    std::vector<LegendsMapEntry> legendsMapsOf(LegendsPlatform platform, bool rescan = false) const;
    /** The folder of the Legends map named `name`: on `platform`, or on whichever has it. */
    std::optional<std::pair<LegendsPlatform, std::filesystem::path>>
    findLegendsMap(std::string_view name, std::optional<LegendsPlatform> platform) const;
    /** Opens the Legends map in `directory` of `platform`, walked when `playable`, else flown
     * through as a preview; `completedTravel` is finishLegendsTravel's own flag. */
    void consoleLegendsOpen(DevConsole& console, LegendsPlatform platform,
                            const std::filesystem::path& directory, bool playable,
                            bool completedTravel = false,
                            std::optional<std::vector<PartyMember>> visitingParty = std::nullopt);
    /** Leaves a Legends map or its preview for the level left for it. */
    void consoleLegendsClose(DevConsole& console);
    void updateLegendsPreview(f64 deltaSeconds);
    bool commitLegendsProgress();
    bool closeLegendsLevel(bool keepProgress = true);
    bool quitLegendsLevel();
    void finishLegendsTravel();
    void consoleUnlockTower(DevConsole& console);
    void consoleGet(DevConsole& console, DevConsole::Arguments arguments);
    void consoleHideHud(DevConsole& console);
    void consoleFreeCam(DevConsole& console);
    /** Picks what stands under `cursor` in the view for the console, or lets the selection
     * go without one; the console's $SELECTED follows. */
    void consoleSelect(std::optional<Vec2> cursor);
    /** Whether the console's screen, as far down as it has slid, lies under `cursor` (0 to
     * 1 across and down the window): the 2D layer's letterboxed frame, its top rows. */
    bool consoleCovers(Vec2 cursor);
    /** What the console says of a thing selected or named: its token, name, place and
     * what else is known of its kind. */
    std::vector<std::string> consoleDescribe(const SceneSelection& selection);
    /** Carries the console's cheats, hidden HUD and free camera into the scene each tick. */
    void applyConsoleState(f64 deltaSeconds, bool consoleOpen);
    /** Takes the party to `level` through the loading picture, standing it at `position`
     * facing `yaw` when given; the scene closes at once. */
    void consoleTravel(const LevelRef& level, std::optional<Vec3> position, std::optional<f32> yaw);
    void resetPlayInput();
    void updatePause(f64 deltaSeconds);
    bool saveSettings(const GameConfig& config);
    bool applySettings(const GameConfig& config, bool persist);
    void updateAfterLevel(f64 deltaSeconds);
    bool openImportedShop(ShopVisit visit);
    void finishJourney();
    void updateJourney(f64 deltaSeconds);
    bool updateCompletion(f64 deltaSeconds);
    void autoSave(std::span<PartyMember> party);
    bool saveLocalParty(std::span<PartyMember> party);
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
    LevelDebugOverlay m_levelDebug;
    DevConsole m_console;
    FreeCamera m_freeCamera;
    LegendsPreview m_legendsPreview;
    mutable std::array<std::optional<std::vector<LegendsMapEntry>>, 2> m_legendsMaps;
    std::array<NoclipMotion, static_cast<usize>(PlayScene::kPlayerCount)> m_noclip;
    std::array<PlayScene::ConsoleCheats, static_cast<usize>(PlayScene::kPlayerCount)>
        m_consoleCheats{};
    std::optional<s32> m_partyCountOverride; ///< the console's party_count, across levels
    bool m_hudHidden = false;
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
    /** Loads `table` in `language` from the game's text, then each mechanic's over it. */
    bool loadText(StringTable& table, std::string_view language) const;
    StringTable m_strings;
    MechanicCatalog m_mechanics;
    std::unique_ptr<AudioDevice> m_audio;
    std::unique_ptr<SoundPlayer> m_sounds;
    std::unique_ptr<SoundPlayer> m_legendsSounds;
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
    std::unique_ptr<PlaySession>
        m_legendsOrigin; ///< paused GDL scene during an offline native visit
    std::vector<PartyMember> m_legendsEntryParty;
    std::vector<PartyMember> m_legendsTowerParty;        ///< last installed native Tower collection
    std::shared_ptr<LegendsAdviceState> m_legendsAdvice; ///< native new-party advice scope
    PauseMenu m_pause;
    MusicDuck m_musicDuck; ///< the music under the pause menu
    AfterLevelScene m_afterLevel;
    AutoSaveIcon m_autoSaveIcon;
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
        bool returningFromChallenge = false;
        bool resumeImported = false; ///< an in-place menu, not map travel or level completion
    };
    std::optional<Journey> m_journey;
    void returnFromChallenge(std::span<const PartyMember> party);
    SaveSlots m_saves; ///< where the party in play is kept
    OfflineConsoleCommands m_offlineConsole{offlineConsolePorts()};
    TransitionScreen m_loadingPicture;
    LevelLoadingScreen m_levelLoading;
    Canvas m_canvas;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    bool m_titleWarned = false;
};

} // namespace gdl::game
