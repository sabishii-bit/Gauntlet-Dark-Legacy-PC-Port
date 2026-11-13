#pragma once
#include <array>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/app/Application.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioDevice.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/platform/DisplayMode.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "expansions/shared/scenes/LegendsMapCatalog.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "game/app/AttractSequencer.h"
#include "game/app/BuildLabel.h"
#include "game/app/CommandLine.h"
#include "game/app/ConsoleActionRoute.h"
#include "game/app/LevelCompletion.h"
#include "game/app/LevelDebugOverlay.h"
#include "game/app/OnlineRun.h"
#include "game/audio/MusicDuck.h"
#include "game/camera/FreeCamera.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/DevConsole.h"
#include "game/console/NoclipMotion.h"
#include "game/context/GameServices.h"
#include "game/hud/AutoSaveIcon.h"
#include "game/hud/ControlPrompts.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/menu/LiveSettings.h"
#include "game/menu/PauseMenu.h"
#include "game/online/PartyStore.h"
#include "game/party/PlayInputReader.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/AttractScene.h"
#include "game/scenes/ClassUnlocks.h"
#include "game/scenes/Haptics.h"
#include "game/scenes/IdleScreen.h"
#include "game/scenes/LevelLoadingScreen.h"
#include "game/scenes/MovieScene.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayerSelectScene.h"
#include "game/scenes/SmokeTestScene.h"
#include "game/scenes/TitleScene.h"
#include "game/tower/LevelExitSpeech.h"
#include "game/widgets/SceneInput.h"

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
    /** The controllers' rumble the screens and the online game ask for, through the window
     * while it stands. */
    class RumbleRouting final : public Haptics {
    public:
        explicit RumbleRouting(Gauntlet& game) : m_game(game) {}
        void vibrate(s32 player, s32 frames, ControlFeedback cue) override;
        void stopRumble() override;

    private:
        Gauntlet& m_game;
    };
    /** Grants a won challenge's classes into the saves' shared unlock profile. */
    class UnlockRouting final : public ClassUnlocks {
    public:
        explicit UnlockRouting(Gauntlet& game) : m_game(game) {}
        void unlockClasses(u16 mask) override;

    private:
        Gauntlet& m_game;
    };
    /** Saves the online game's local characters as the party in play is saved. */
    class PartyRouting final : public PartyStore {
    public:
        explicit PartyRouting(Gauntlet& game) : m_game(game) {}
        void autoSave(std::span<PartyMember> party) override;
        bool saveParty(std::span<PartyMember> party) override;

    private:
        Gauntlet& m_game;
    };
    /** The controls' names every screen's prompts show, from the bindings and the device each
     * player last used. A shared prompt (-1) speaks to the player who last pressed something,
     * or, when the offline level in play has no body for them, to its first who has one: that
     * fallback consults the offline scene for every asker, the online game's screens too. */
    class LabelRouting final : public ControlLabels {
    public:
        explicit LabelRouting(Gauntlet& game) : m_game(game) {}
        std::string label(s32 player, std::string_view action) const override;

    private:
        Gauntlet& m_game;
    };
    /** The settings the title and pause menus edit: written to the settings file and applied,
     * tried on the window and heard through the mixers. */
    class SettingsRouting final : public LiveSettings {
    public:
        explicit SettingsRouting(Gauntlet& game) : m_game(game) {}
        bool save(const GameConfig& next) override;
        bool previewVideo(const GameConfig& trial) override;
        void previewAudio(const AudioConfig& audio) override;
        DisplayOptions display() const override;
        GraphicsApi renderer() const override;

    private:
        Gauntlet& m_game;
    };

    bool startMovie(std::string_view name);
    bool startTitleScreen();
    void startNextAttractScreen();
    // Each screen's update reads its devices through the one SceneInput onUpdate makes.
    void updateMovie(f64 deltaSeconds, const SceneInput& sceneInput);
    void updateAttract(f64 deltaSeconds, const SceneInput& sceneInput);
    bool updateIdle(f64 deltaSeconds);
    void updateTitle(f64 deltaSeconds, const SceneInput& sceneInput);
    void updateSelect(f64 deltaSeconds);
    void updateNetplay(f64 deltaSeconds, const SceneInput& sceneInput);
    void connectNetplay();
    void leaveNetplay();
    void updateTower(f64 deltaSeconds, const SceneInput& sceneInput);
    /** Every input slot's play controls through m_playInput, each cursor aimed in the scene
     * its slot plays; online each slot's noclip flight too. */
    PlayScene::Inputs readPlayInputs(f64 deltaSeconds, const SceneInput& sceneInput);
    /** The level debug overlay's lines for the level in play. */
    std::vector<std::string> levelDebugLines() const;
    /** The developer console's font, banner and commands (GauntletConsole.cpp). */
    void setUpConsole();
    /** What the console's policy judges by: who is at it, whether it may run commands, and
     * the words a Legends preview admits. Set before every console update and draw. */
    ConsoleSession consoleSession() const;
    const PlayScene* consoleScene() const;
    const LevelWorld* consoleWorld() const;
    /** Where the shared console verbs' lines go: the online game's request for the console's
     * seat, else the next offline step. */
    ConsoleActionRoute consoleActionRoute();
    bool updateOnlineConsole(f64 deltaSeconds, bool overlay = false);
    /** The player the console speaks for: the one at the keyboard, else the first in; -1
     * with nobody in. */
    s32 consolePlayer();
    /** Registers the Legends expansion's console words over this application: goto's maps
     * and the way back from one, unlock_tower in a native world walked, and list maps' Legends
     * lines (GauntletLegends.cpp). */
    void registerLegendsConsole(ConsoleHost& host, ConsoleListings& listings);
    /** The only words the console runs while a Legends map's preview holds the scene; nothing
     * while no preview is open. */
    std::optional<std::span<const std::string_view>> legendsConsoleHold() const;
    /** unlock_tower in a native Legends world walked: its own tower's unlocks for the party. */
    void consoleLegendsUnlockTower(DevConsole& console);
    /** The Legends maps installed for a platform, as they are kept. The disk is walked on the
     * first ask and whenever `rescan` asks (list maps, goto); the console's completion reads
     * what was found, so typing costs nothing when there is no Legends content at all. */
    const std::vector<LegendsMapEntry>& foundLegendsMaps(LegendsPlatform platform,
                                                         bool rescan) const;
    /** Whether either platform has a Legends map installed, found as foundLegendsMaps finds
     * them; nothing is copied, so help and the suggestions may ask it every frame. */
    bool legendsMapsInstalled(bool rescan) const;
    /** The folder of the Legends map named `name`: on `platform`, or on whichever has it. */
    std::optional<std::pair<LegendsPlatform, std::filesystem::path>>
    findLegendsMap(std::string_view name, std::optional<LegendsPlatform> platform) const;
    /** Opens the Legends map in `directory` of `platform`, walked when `playable`, else flown
     * through as a preview; `completedTravel` is finishLegendsTravel's own flag. */
    void consoleLegendsOpen(DevConsole& console, LegendsPlatform platform,
                            const std::filesystem::path& directory, bool playable,
                            bool completedTravel = false,
                            std::optional<std::vector<PartyMember>> visitingParty = std::nullopt,
                            bool completionRecorded = false);
    /** Leaves a Legends map or its preview for the level left for it. */
    void consoleLegendsClose(DevConsole& console);
    void updateLegendsPreview(f64 deltaSeconds);
    bool commitLegendsProgress();
    bool closeLegendsLevel(bool keepProgress = true);
    bool quitLegendsLevel();
    void finishLegendsTravel();
    /** Closes the parked native scene once the selector leaves a completed Legends journey.
     * The shop or the selector saved its party; the parked actors still hold pre-shop values,
     * which a later shutdown must not take for an abandoned live level. */
    void closeCompletedLegendsJourney();
    /** At shutdown during a completed Legends journey, saves the party of the menu that is
     * open, purchases and save slots included, instead of the parked scene's; false, saving
     * nothing, without such a journey. */
    bool saveLegendsCompletion();
    /** Picks what stands under `cursor` in the view for the console, or lets the selection
     * go without one; the console's $SELECTED follows. */
    void consoleSelect(std::optional<Vec2> cursor);
    /** Whether the console's screen, as far down as it has slid, lies under `cursor` (0 to
     * 1 across and down the window): the 2D layer's letterboxed frame, its top rows. */
    bool consoleCovers(Vec2 cursor);
    /** Carries the console's hidden HUD and free camera into the scene each tick. */
    void applyConsoleState(f64 deltaSeconds, bool consoleOpen);
    /** The noclip wish input slot `slot` flies this frame; none while its seat's noclip is off
     * or the console is down. */
    Vec3 consoleFlight(s32 slot, f64 seconds, bool consoleOpen);
    /** Stops every input slot's noclip flight. */
    void resetConsoleFlights();
    /** Takes the party where the stepped scene's console travel request leads, by the rule
     * both play modes share: into a challenge the level left waits as its parent, back to that
     * parent it takes up again at the request's place, anywhere else the scene closes behind
     * the loading picture. Nothing is won, saved or recorded. */
    void travelByConsole();
    void resetPlayInput();
    void updatePause(f64 deltaSeconds, const SceneInput& sceneInput);
    bool saveSettings(const GameConfig& config);
    bool applySettings(const GameConfig& config, bool persist);
    void updateAfterLevel(f64 deltaSeconds, const SceneInput& sceneInput);
    bool openImportedShop(ShopVisit visit);
    void finishJourney();
    void updateJourney(f64 deltaSeconds, const SceneInput& sceneInput);
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
    static s32 playerPressingStart(const SceneInput& sceneInput);
    /** The view every screen is opened with, its levels read on the first ask. */
    const GameServices& services();
    /** What a play scene answers its rumble, won classes and Stop Time total through. */
    PlayScene::Links playLinks();
    /** What the online game answers its rumble, won classes and saves through. */
    OnlineRun::Links onlineLinks();
    void applyWindowIcon();
    void renderScene(RenderDevice& device);
    /** Blacks out the display above and below the frame, whatever a screen drew there, as the
     * GameCube's video border shows its 640x448 frame within the 480-line picture
     * (GXAdjustForOverscan by 16 lines at 0x800CBA80); beside the frame a wide display goes on
     * showing the view. */
    void drawFrameBorder(RenderDevice& device);

    GameOptions m_options;
    GameConfig m_config;
    BuildLabel m_buildLabel;
    LevelDebugOverlay m_levelDebug;
    ConsoleActionRoute m_consoleRoute{consoleActionRoute()}; ///< outlives the console's commands
    ConsoleListings m_consoleListings; ///< what list names; outlives the console's commands
    DevConsole m_console;
    FreeCamera m_freeCamera;
    LegendsPreview m_legendsPreview;
    mutable std::array<std::optional<std::vector<LegendsMapEntry>>, 2> m_legendsMaps;
    std::array<NoclipMotion, static_cast<usize>(PlayScene::kPlayerCount)> m_noclip;
    /** Each seat's console cheats as the scene stepped last holds them, carried into the next
     * scene opened or taken up again. */
    std::array<PlayScene::ConsoleCheats, static_cast<usize>(PlayScene::kPlayerCount)>
        m_consoleCheats{};
    std::optional<s32> m_partyCountOverride; ///< the console's party_count, across levels
    bool m_hudHidden = false;
    std::string m_version;
    RumbleRouting m_rumble{*this}; ///< outlives every scene and online game that rumbles
    UnlockRouting m_unlocks{*this};
    PartyRouting m_partyStore{*this};
    SettingsRouting m_liveSettings{*this}; ///< outlives the title and pause menus that edit it
    LabelRouting m_controlLabels{*this};   ///< outlives every screen that shows a prompt
    /** What every screen reads, held by each from its opening: declared before them all. The
     * sounds, movie mixer and files are filled in as onInit makes them, and let go before
     * onShutdown releases them. */
    GameServices m_services{.config = &m_config,
                            .strings = &m_strings,
                            .controlLabels = &m_controlLabels,
                            .levels = &m_levels,
                            .mechanics = &m_mechanics,
                            .unpackedRoot = m_options.unpackedDirectory};
    bool m_windowLive = true; ///< until shutdown has closed every screen; the window goes next
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
    PlayInputReader m_playInput;
    SessionInputs m_sessionInputs;
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
        struct ImportedDestination {
            LegendsPlatform platform;
            std::filesystem::path directory;
        };
        std::optional<ImportedDestination> importedDestination;
    };
    std::optional<Journey> m_journey;
    void returnFromChallenge(std::span<const PartyMember> party);
    /** Opens a completed Legends journey's native destination. When it does not open, the
     * journey is kept, with its purchases and save slots, and the after-level menu opens
     * again for its Done to retry; neither the tally nor the completion runs twice. */
    void finishImportedJourney(Journey journey);
    SaveSlots m_saves; ///< where the party in play is kept
    TransitionScreen m_loadingPicture;
    LevelLoadingScreen m_levelLoading;
    Canvas m_canvas;
    SmokeTestScene m_smokeTest;
    bool m_movieActive = false;
    bool m_titleWarned = false;
};

} // namespace gdl::game
