#include "game/app/Gauntlet.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/PngImage.h"
#include "engine/assets/WorldData.h"
#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/platform/Paths.h"
#include "engine/render/RenderTypes.h"

#include "expansions/shared/app/LegendsCompletion.h"
#include "expansions/shared/characters/LegendsVisit.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "formats/TplFile.h"
#include "game/app/GameCursor.h"
#include "game/app/ImportedShop.h"
#include "game/app/Scenario.h"
#include "game/characters/AutoSave.h"
#include "game/config/ControlProfiles.h"
#include "game/enemies/Enemies.h"
#include "game/party/PlayInputReader.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/ConsoleTravel.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/SceneInput.h"

namespace gdl::game {

namespace {

constexpr std::string_view kMovieDirectory = "VQMOVIES";
constexpr std::string_view kTextDirectory = "text";
constexpr std::string_view kModDirectory = "mods";             ///< data/mods/<mechanic id>/text
constexpr std::string_view kWindowIcon = "carddemo/icon0.png"; ///< unpacked memory-card icon
/** The title screen takes a Start from every input slot to the select screen. */
constexpr auto kEverySlot = [] {
    std::array<bool, kInputSlots> admitted{};
    admitted.fill(true);
    return admitted;
}();

/** Offline, an input slot's cursor turns the scene's player of the same number. */
class OfflineAim final : public PlayInputReader::Aim {
public:
    explicit OfflineAim(const PlayScene& scene) : m_scene(scene) {}
    std::optional<Vec3> toward(s32 inputSlot, Vec2 cursor) const override {
        if (const auto* actor = m_scene.actor(inputSlot)) {
            return m_scene.cursorAim(cursor, actor->position());
        }
        return std::nullopt;
    }

private:
    const PlayScene& m_scene;
};

/** Online, an input slot's cursor turns the seat its device holds in the room's scene. */
class OnlineAim final : public PlayInputReader::Aim {
public:
    explicit OnlineAim(const OnlineRun& run) : m_run(run) {}
    std::optional<Vec3> toward(s32 inputSlot, Vec2 cursor) const override {
        return m_run.cursorAim(inputSlot, cursor);
    }

private:
    const OnlineRun& m_run;
};

} // namespace

Gauntlet::Gauntlet(ApplicationDesc desc, GameOptions options, GameConfig config,
                   std::string_view version, MechanicCatalog mechanics)
    : Application(std::move(desc)), m_options(std::move(options)), m_config(std::move(config)),
      m_buildLabel(version), m_version(version), m_mechanics(std::move(mechanics)) {}

bool Gauntlet::loadText(StringTable& table, std::string_view language) const {
    if (!table.load(m_options.dataDirectory / kTextDirectory, language)) {
        return false;
    }
    // A mechanic's own text keeps ids of its own, so none of it can replace the game's.
    for (const MechanicDescriptor& mechanic : m_mechanics.all()) {
        table.layer(m_options.dataDirectory / kModDirectory / mechanic.id / kTextDirectory);
    }
    return true;
}

/** Use the original sibling carddemo icon, retaining PNG support for exported fixtures. */
void Gauntlet::applyWindowIcon() {
    const auto native =
        AssetLocator(m_options.unpackedDirectory.parent_path()).find("carddemo/icon.tpl");
    const auto exported = AssetLocator(m_options.unpackedDirectory).find(kWindowIcon);
    if (!native && !exported) {
        return;
    }
    try {
        Image icon;
        if (native) {
            auto images = formats::parseTplFile(readFile(*native));
            if (images.empty()) {
                return;
            }
            icon = std::move(images.front().image);
        } else {
            icon = loadImageFile(*exported);
        }
        window().setIcon(std::span<const Image>(&icon, 1));
    } catch (const std::exception& e) {
        log::warn("Window icon unusable: {}", e.what());
    }
}

void Gauntlet::onInit() {
    renderDevice().setSmoothSprites(m_config.display.smoothSprites);
    renderDevice().setAuthoredSilhouettes(m_config.display.authoredSilhouettes);
    // A saved exclusive resolution may no longer exist after moving displays.
    m_config.display.windowMode = window().windowMode();
    applyWindowIcon();
    if (const auto cursor = GameCursor::load(m_options.unpackedDirectory)) {
        if (!window().setCursor(cursor->image, cursor->hotX, cursor->hotY)) {
            log::warn("Custom mouse cursor unavailable; retaining the system pointer");
        }
    }
    m_buildLabel.load(renderDevice());
    setUpConsole();
    m_autoSaveIcon.load(renderDevice(), m_options.unpackedDirectory);
    if (!loadText(m_strings, m_config.text.language)) {
        log::warn("No text tables under {}; identifiers will show instead of text",
                  paths::utf8(m_options.dataDirectory / kTextDirectory));
    }
    m_audio = std::make_unique<AudioDevice>();
    m_audio->mixer().setStereo(m_config.audio.stereo);
    m_sounds = std::make_unique<SoundPlayer>(m_audio->mixer());
    m_sounds->setMasterVolume(m_config.audio.masterVolume);
    m_sounds->setCategoryVolume(SoundCategory::Music, m_config.audio.musicVolume);
    m_sounds->setCategoryVolume(SoundCategory::Effects, m_config.audio.effectsVolume);
    m_sounds->setCategoryVolume(SoundCategory::Movie, m_config.audio.movieVolume);
    m_assets = std::make_unique<AssetLocator>(assetDirectory());
    m_services.sounds = m_sounds.get();
    m_services.movieMixer = &m_audio->mixer();
    m_services.assets = m_assets.get();
    m_smokeTest.init(renderDevice());

    if (!m_options.netplayTest.empty()) {
        setUpdateRate(60);
        m_online = std::make_unique<OnlineRun>();
        try {
            const auto scenario = Scenario::load(m_options.scenario);
            const auto& game = services();
            const auto level = scenario.level.empty() ? std::optional{LevelRef::tower()}
                                                      : m_levels.byName(scenario.level);
            if (scenario.afterLevel || scenario.ending || !level ||
                !m_online->open(renderDevice(), game, onlineLinks(), m_options.netplayTest,
                                m_options.netplayRoom, m_version, m_options.netplayContent,
                                scenario.partyMembers(), *level, scenario.tower,
                                m_options.netplayAutoStart, m_options.netplayInvite)) {
                log::error("Online scene test could not start");
                requestQuit();
            }
        } catch (const std::exception& error) {
            log::error("Online scene test: {}", error.what());
            requestQuit();
        }
        return;
    }

    if (m_options.previewScreensaver) {
        m_idleWatch.update(IdleWatch::kWaitSeconds, input(), true);
        if (!m_idleScreen.open(renderDevice(), m_options.unpackedDirectory)) {
            requestQuit();
        }
        return;
    }
    if (m_options.startAtDemo) {
        m_attract.titleShown();
        startNextAttractScreen();
        return;
    }

    if (!m_options.playMovie.empty()) {
        if (!startMovie(m_options.playMovie)) {
            requestQuit();
        }
        return;
    }
    if (!m_options.scenario.empty()) {
        if (!startScenario(m_options.scenario)) {
            requestQuit();
        }
        return;
    }
    if (m_options.startAtTitle && startTitleScreen()) {
        return;
    }
    startNextAttractScreen();
}

/** Opens the tower onto the start a scenario file describes; false (with the reason logged)
 * when the file or the level cannot be used. */
bool Gauntlet::startScenario(const std::filesystem::path& file) {
    try {
        const Scenario scenario = Scenario::load(file);
        log::info("Scenario {}: {} in the party", paths::utf8(file), scenario.party.size());
        if (scenario.afterLevel) {
            const auto& game = services();
            const auto levelName = scenario.level.empty() ? "G1" : scenario.level;
            const auto reference = m_levels.byName(levelName);
            WorldData world;
            if (!reference ||
                !world.load(m_options.unpackedDirectory / reference->worldDataFile())) {
                log::error("After-level scenario: no realm data for {}", levelName);
                return false;
            }
            const auto* level = world.level(levelName);
            if (level == nullptr) {
                log::error("After-level scenario: no level data for {}", levelName);
                return false;
            }
            Journey journey;
            journey.destination = LevelRef::tower();
            journey.party = scenario.partyMembers();
            journey.options.welcome = false;
            journey.options.arriving = true;
            journey.options.arrivalWorld = static_cast<u32>(std::max(reference->realmId, 0));
            if (scenario.ending) {
                const LevelCompletion flow(level->bossType, true);
                if (flow.movie().empty()) {
                    log::error("Ending scenario requires Garm or Underworld Skorne");
                    return false;
                }
                recordLevelBeaten(journey.party, reference->realmId, reference->index, level->rune,
                                  level->legend);
                journey.completion =
                    Journey::Completion{flow, scenario.results, level->shopMaxima, levelName};
            } else if (!m_afterLevel.open(renderDevice(), game, journey.party, scenario.results,
                                          level->shopMaxima, levelName, scenario.shopVisit)) {
                return false;
            }
            m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
            m_loadingPicture.cover();
            m_journey = std::move(journey);
            return true;
        }
        if (!scenario.level.empty()) {
            if (!m_levels.loaded()) {
                m_levels.load(m_options.unpackedDirectory);
            }
            const auto level = m_levels.byName(scenario.level);
            if (!level.has_value()) {
                log::error("Scenario {}: no level named {}", paths::utf8(file), scenario.level);
                return false;
            }
            return startLevel(*level, scenario.partyMembers(), scenario.tower);
        }
        return startTower(scenario.partyMembers(), scenario.tower);
    } catch (const std::exception& e) {
        log::error("Scenario {}: {}", paths::utf8(file), e.what());
        return false;
    }
}

const GameServices& Gauntlet::services() {
    if (!m_levels.loaded()) {
        m_levels.load(m_options.unpackedDirectory);
    }
    return m_services;
}

std::string Gauntlet::LabelRouting::label(s32 player, std::string_view action) const {
    if (player < 0) {
        player = m_game.m_promptDevices.lastPlayer();
        if (m_game.m_play->scene.isOpen() && m_game.m_play->scene.actor(player) == nullptr) {
            for (s32 candidate = 0; candidate < PlayScene::kPlayerCount; ++candidate) {
                if (m_game.m_play->scene.actor(candidate) != nullptr) {
                    player = candidate;
                    break;
                }
            }
        }
    }
    return m_game.m_promptDevices.label(m_game.input(), m_game.m_config, player, action);
}

PlayScene::Links Gauntlet::playLinks() {
    return {.haptics = &m_rumble, .unlocks = &m_unlocks, .stopTimeTotal = &m_stopTimeTotal};
}

OnlineRun::Links Gauntlet::onlineLinks() {
    return {.haptics = &m_rumble, .unlocks = &m_unlocks, .store = &m_partyStore};
}

void Gauntlet::RumbleRouting::vibrate(s32 player, s32 frames, ControlFeedback cue) {
    if (!m_game.m_windowLive) {
        return;
    }
    if (const auto request =
            controlVibration(m_game.m_config, m_game.input(), player, frames, cue)) {
        m_game.window().rumble(request->pad, request->low, request->high, request->milliseconds,
                               request->priority);
    }
}

void Gauntlet::RumbleRouting::stopRumble() {
    if (m_game.m_windowLive) {
        m_game.window().stopRumble();
    }
}

void Gauntlet::UnlockRouting::unlockClasses(u16 mask) {
    // Only newly earned rewards come here; preloaded scenario relics do not.
    auto& saves = m_game.m_saves;
    if (saves.opened() || saves.open(m_game.m_config.saveDirectory(), m_game.m_config.save.slots)) {
        saves.unlockClasses(mask);
    }
}

void Gauntlet::PartyRouting::autoSave(std::span<PartyMember> party) {
    m_game.autoSave(party);
}

bool Gauntlet::PartyRouting::saveParty(std::span<PartyMember> party) {
    return m_game.saveLocalParty(party);
}

bool Gauntlet::SettingsRouting::save(const GameConfig& next) {
    return m_game.saveSettings(next);
}

bool Gauntlet::SettingsRouting::previewVideo(const GameConfig& trial) {
    return m_game.applySettings(trial, false);
}

void Gauntlet::SettingsRouting::previewAudio(const AudioConfig& audio) {
    m_game.m_sounds->setCategoryVolume(SoundCategory::Music, audio.musicVolume);
    m_game.m_sounds->setCategoryVolume(SoundCategory::Effects, audio.effectsVolume);
    m_game.m_sounds->setCategoryVolume(SoundCategory::Movie, audio.movieVolume);
    m_game.m_movie.setVolume(audio.masterVolume * audio.movieVolume);
    m_game.m_audio->mixer().setStereo(audio.stereo);
}

DisplayOptions Gauntlet::SettingsRouting::display() const {
    return m_game.window().displayOptions();
}

GraphicsApi Gauntlet::SettingsRouting::renderer() const {
    return m_game.activeRenderer();
}

void Gauntlet::onUpdate(f64 deltaSeconds) {
    m_autoSaveIcon.update(deltaSeconds);
    m_promptDevices.update(input(), m_config);
    // The pointer steps aside while a pad plays and comes back with the mouse or keyboard.
    window().hideCursor(m_promptDevices.cursorHidden());
    // Read when asked, so a settings change made during this update is seen.
    const SceneInput sceneInput{input(), m_config};
    if (m_netplay) {
        updateNetplay(deltaSeconds, sceneInput);
        m_sounds->update();
        return;
    }
    if (m_online) {
        const bool consoleOpen = updateOnlineConsole(deltaSeconds);
        const auto menu = consoleOpen ? MenuInput{} : sceneInput.shared();
        if (menu.escape) {
            requestQuit();
            return;
        }
        if (consoleOpen) {
            resetConsoleFlights(); // a frame that reads no controls flies nobody
        }
        m_online->update(
            consoleOpen ? SessionInputs::Frame{} : readPlayInputs(deltaSeconds, sceneInput), menu);
        if (!m_online->consoleAllowed()) {
            consoleSelect(std::nullopt);
            m_console.close();
            m_freeCamera.stop();
            m_online->consoleCapture(false);
            m_online->consoleView(m_hudHidden, std::nullopt);
            window().holdCursor(false);
        }
        m_sounds->update();
        return;
    }
    if (m_legendsPreview.isOpen()) {
        updateLegendsPreview(deltaSeconds);
        return;
    }
    if (updateIdle(deltaSeconds)) {
        window().stopRumble();
        return;
    }
    // Gameplay and editable menus own Escape; only passive screens treat it as quit.
    if (sceneInput.quitPressed() && !m_play->scene.isOpen() && !m_pause.isOpen() &&
        !m_journey.has_value() && !(m_title.isOpen() && m_title.optionsOpen()) &&
        !(m_select.isOpen() && m_select.typing())) {
        requestQuit();
    }

    if (m_pause.isOpen()) {
        updatePause(deltaSeconds, sceneInput);
    } else if (m_afterLevel.isOpen()) {
        updateAfterLevel(deltaSeconds, sceneInput);
    } else if (m_select.isOpen()) {
        updateSelect(deltaSeconds);
    } else if (m_journey.has_value()) {
        updateJourney(deltaSeconds, sceneInput);
    } else if (m_demo.isOpen()) {
        updateAttract(deltaSeconds, sceneInput);
    } else if (m_movieActive) {
        updateMovie(deltaSeconds, sceneInput);
    } else if (m_title.isOpen()) {
        updateTitle(deltaSeconds, sceneInput);
    } else if (m_play->scene.isOpen()) {
        updateTower(deltaSeconds, sceneInput);
    }
    // The music ducks under the pause menu and comes back once it is left.
    m_musicDuck.update(deltaSeconds, m_pause.isOpen() && !m_pause.musicAudible());
    // The level flybys are silent (sndFxPaused holds every voice through the attract modes).
    m_sounds->setCategoryScale(SoundCategory::Music, m_demo.isOpen() ? 0.0f : m_musicDuck.level());
    m_sounds->setCategoryScale(SoundCategory::Effects, m_demo.isOpen() ? 0.0f : 1.0f);
    if (m_journey && m_journey->importedDestination) {
        if (auto* source = m_play->world.imported())
            source->servicePreservedSpeech(static_cast<f32>(deltaSeconds));
    }
    m_sounds->update();
    if (m_legendsSounds) {
        m_legendsSounds->setMasterVolume(m_config.audio.masterVolume);
        m_legendsSounds->setCategoryVolume(SoundCategory::Music, m_config.audio.musicVolume);
        m_legendsSounds->setCategoryScale(SoundCategory::Music, m_musicDuck.level());
        m_legendsSounds->update();
    }
}

bool Gauntlet::updateIdle(f64 deltaSeconds) {
    const bool eligible = m_options.previewScreensaver ||
                          (!m_movieActive && !m_journey.has_value() && !m_demo.isOpen() &&
                           (!m_title.isOpen() || m_title.menuOpen() || m_title.optionsOpen()) &&
                           !m_play->scene.gameOver().active() &&
                           (m_play->scene.isOpen() || m_select.isOpen() || m_afterLevel.isOpen() ||
                            m_title.isOpen()));
    const bool wasOpen = m_idleScreen.isOpen();
    if (m_idleWatch.update(deltaSeconds, input(), eligible)) {
        if (!wasOpen && !m_idleScreen.open(renderDevice(), m_options.unpackedDirectory)) {
            m_idleWatch.reset();
            return false;
        }
        m_audio->mixer().setPaused(true);
        m_idleScreen.update(deltaSeconds, glm::radians(m_config.camera.horizontalFovDegrees),
                            static_cast<f32>(m_config.display.frameWidth) /
                                static_cast<f32>(m_config.display.frameHeight));
        return true;
    }
    if (wasOpen) {
        m_idleScreen.close();
        m_audio->mixer().setPaused(false);
        resetPlayInput();
        if (m_options.previewScreensaver) {
            requestQuit();
        }
    }
    return wasOpen || m_idleWatch.consumingInput();
}

void Gauntlet::updateAttract(f64 deltaSeconds, const SceneInput& sceneInput) {
    const auto outcome = m_demo.update(deltaSeconds, sceneInput.shared());
    if (outcome == AttractOutcome::Running) {
        return;
    }
    m_demo.close();
    if (outcome != AttractOutcome::Title || !startTitleScreen()) {
        startNextAttractScreen();
    }
}

void Gauntlet::updateMovie(f64 deltaSeconds, const SceneInput& sceneInput) {
    const MenuInput menu = sceneInput.movie();
    const bool toTitle = m_options.playMovie.empty() && m_attract.canSkipToTitle() && menu.start;
    const bool playing = !menu.start && !menu.select && m_movie.update(deltaSeconds);
    if (playing) {
        return;
    }
    m_movie.close();
    m_movieActive = false;
    if (!m_options.playMovie.empty()) {
        requestQuit();
    } else if (!(toTitle && startTitleScreen())) {
        startNextAttractScreen();
    }
}

void Gauntlet::updateTitle(f64 deltaSeconds, const SceneInput& sceneInput) {
    const TitleOutcome outcome = m_title.update(deltaSeconds, sceneInput.shared());
    if (outcome == TitleOutcome::Running) {
        return;
    }
    if (outcome == TitleOutcome::Quit) {
        requestQuit();
        return;
    }
    if (outcome == TitleOutcome::Netplay) {
        m_netplay = std::make_unique<NetplayNavigation>();
        m_netplay->view.available = OnlineRun::available();
        if (!OnlineRun::available()) {
            m_netplay->notice = "Netplay is not included in this build.";
            m_netplay->view.status = m_netplay->notice;
        }
        m_netplay->menu.show(renderDevice(), m_netplay->view);
        return;
    }
    if (outcome == TitleOutcome::StartGame && startPlayerSelect(playerPressingStart(sceneInput))) {
        m_title.releaseMusic();
        m_title.close();
        m_select.join(sceneInput.joining(kEverySlot));
        return;
    }
    m_title.close();
    startNextAttractScreen();
}

/** The player whose Start or Select is down this frame; the first when none is. */
s32 Gauntlet::playerPressingStart(const SceneInput& sceneInput) {
    for (s32 player = 0; player < PlayerSelectScene::kLaneCount; ++player) {
        const MenuInput menu = sceneInput.slot(player);
        if (menu.start || menu.select || menu.escape) {
            return player;
        }
    }
    return 0;
}

bool Gauntlet::startPlayerSelect(s32 startingPlayer, std::span<const PartyMember> party,
                                 bool manage) {
    if (m_select.open(renderDevice(), services(), startingPlayer, party, manage,
                      m_title.musicHandle(), &m_play->world)) {
        // Native game-main state8 calls e0070d2c on entering player selection.
        // Failed selection loads and ordinary map/quit transitions do not reset it.
        if (m_legendsAdvice) {
            *m_legendsAdvice = {};
        }
        return true;
    }
    log::warn("Player select unavailable; unpack the game data into {} with gdlunpack",
              paths::utf8(m_options.unpackedDirectory));
    return false;
}

void Gauntlet::updateSelect(f64 deltaSeconds) {
    const auto inputs = m_select.readInputs(input());
    const SelectOutcome outcome = m_select.update(deltaSeconds, inputs);
    if (outcome == SelectOutcome::Running) {
        return;
    }
    std::vector<PartyMember> party = m_select.party();
    m_select.close();
    if (m_netplay && m_online) {
        if (outcome == SelectOutcome::Done && !m_online->select(party)) {
            m_netplay->notice =
                std::format("Choose exactly {} local character(s).", m_netplay->view.localPlayers);
        }
        m_netplay->overlay = true;
        resetPlayInput();
        return;
    }
    if (m_journey.has_value()) {
        if (outcome == SelectOutcome::Done) {
            m_journey->party = std::move(party);
            if (m_journey->completion) {
                m_journey->completion->flow.advance();
            }
            finishJourney();
            return;
        }
        if (m_journey->importedDestination) {
            // The completed party was saved by the shop or by this selector.
            // Its parked actors still contain pre-shop values: never let a
            // later shutdown mistake them for an abandoned live level.
            renderDevice().waitIdle();
            m_play->scene.close();
            m_play->world.clear();
            m_legendsOrigin.reset();
            m_legendsEntryParty.clear();
            m_legendsTowerParty.clear();
            if (m_legendsSounds) {
                m_legendsSounds->stopAll();
                m_legendsSounds.reset();
            }
        }
        m_journey.reset();
        m_loadingPicture.release();
        m_levelLoading.close();
    }
    if (outcome == SelectOutcome::Done && startTower(party)) {
        return;
    }
    if (!startTitleScreen()) {
        startNextAttractScreen();
    }
}

bool Gauntlet::startLevel(const LevelRef& level, std::span<const PartyMember> party,
                          const PlayOptions& options) {
    if (m_legendsOrigin && level.realmId >= 0) {
        // An explicit ordinary-level console command ends the visit. Its supplied
        // party is authoritative; do not leave a stale parked scene or soundtrack.
        m_legendsOrigin.reset();
        m_legendsEntryParty.clear();
        m_legendsTowerParty.clear();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_play->scene.close();
    }
    if (!level.isSecret()) {
        m_parent.reset();
    }
    if (party.empty()) {
        return false;
    }
    if (!m_play->world.built() || !(m_play->world.ref() == level)) {
        if (!m_play->world.load(renderDevice(), m_options.unpackedDirectory, level)) {
            log::warn("Level {} is unavailable; unpack it with gdlunpack --only", level.name);
            return false;
        }
    }
    PlayOptions opened = options;
    opened.partyCount = m_partyCountOverride; // the console's party count outlasts a level
    opened.consoleCheats = m_consoleCheats;   // and so do its cheats
    if (m_play->scene.open(renderDevice(), services(), m_play->world, party, opened, playLinks())) {
        resetPlayInput();
        log::info("Entering {} ({})", level.name, level.title);
        setMaxFrameRate(m_config.timing.gameplayFrameRate);
        return true;
    }
    return false;
}

bool Gauntlet::startTower(std::span<const PartyMember> party, const PlayOptions& options) {
    if (party.empty()) {
        return false;
    }
    if (m_play->world.built() && !m_play->world.isTower()) {
        return startLevel(LevelRef::tower(), party, options);
    }
    PlayOptions opened = options;
    opened.partyCount = m_partyCountOverride;
    opened.consoleCheats = m_consoleCheats;
    if (m_play->scene.open(renderDevice(), services(), m_play->world, party, opened, playLinks())) {
        resetPlayInput();
        log::info("Every player is ready; entering the tower");
        setMaxFrameRate(m_config.timing.gameplayFrameRate);
        return true;
    }
    log::warn("The tower is unavailable; unpack the levels with gdlunpack --levels");
    return false;
}

bool Gauntlet::joinTower(const PlayerSelectScene::Inputs& joining) {
    std::vector<PartyMember> party = m_play->scene.party();
    if (!startPlayerSelect(-1, party)) {
        return false;
    }
    m_select.join(joining);
    m_play->scene.close();
    resetPlayInput();
    for (usize player = 0; player < joining.size(); ++player) {
        if (joining[player].start) {
            log::info("Player {} comes to join the party", player + 1);
        }
    }
    return true;
}

void Gauntlet::resetPlayInput() {
    m_playInput.reset();
    m_sessionInputs.beginEpoch();
}

void Gauntlet::updateTower(f64 deltaSeconds, const SceneInput& sceneInput) {
    // The developer console holds every control while it is down: nothing typed at it
    // joins, pauses or moves anyone, and the party stands until it goes back up. The keys
    // of the frame that sends it up are still its: the Escape that closes it opens no menu.
    const bool consoleWasOpen = m_console.open();
    if (const PlayerActor* actor = m_play->scene.actor(consolePlayer())) {
        m_console.setPrompt(actor->save().name + ">");
    }
    m_console.setSession(consoleSession());
    m_console.update(deltaSeconds, input(), m_config.menu.console);
    // The shared verbs' lines typed this frame run in this frame's step alone; a scene a line
    // has sent the party from takes none.
    const std::vector<ConsoleAction> actions = m_consoleRoute.takeOffline();
    if (m_legendsPreview.isOpen()) {
        return;
    }
    const bool consoleOpen = m_console.open() || consoleWasOpen;
    if (m_console.open() && !consoleWasOpen) {
        resetPlayInput();
    }
    // A click in the view under the console picks what stands there; the console going
    // up lets it go.
    if (m_console.open()) {
        // Over the screen the pointer, once it moves, sees through the console and a click
        // there picks what is beneath; elsewhere a click picks as before.
        const PointerSnapshot& pointer = input().pointer();
        const Vec2 cursor{pointer.x, pointer.y};
        const bool over = pointer.inside && consoleCovers(cursor);
        m_console.hover(cursor, over);
        if (input().wasPointerPressed() && pointer.inside && (!over || m_console.seeThrough())) {
            consoleSelect(cursor);
        }
    } else if (consoleWasOpen) {
        consoleSelect(std::nullopt);
    }
    if (!m_play->scene.isOpen()) {
        return; // a command has sent the party elsewhere
    }
    applyConsoleState(deltaSeconds, consoleOpen);
    // Start on a controller the party does not hold, in the tower, brings everyone to the
    // select screen with that player, and the tower back with the party grown
    // (check_active_players, init_player_select(1)).
    std::array<bool, kInputSlots> admitted{};
    for (s32 player = 0; player < PlayScene::kPlayerCount && !consoleOpen; ++player) {
        admitted[static_cast<usize>(player)] = m_play->scene.canJoin(player);
    }
    const MenuInputs joining = sceneInput.joining(admitted);
    if (std::ranges::any_of(joining, &MenuInput::start) && joinTower(joining)) {
        return;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount && !consoleOpen; ++player) {
        if (!m_play->scene.canPause(player)) {
            continue;
        }
        const auto menu = sceneInput.slot(player);
        if (!m_play->scene.leaving() &&
            ((menu.start && !menu.select) || menu.escape || sceneInput.recoveryPressed()) &&
            m_pause.open(renderDevice(), services(), m_play->scene.party(), player, &m_liveSettings,
                         &m_play->world)) {
            m_play->scene.pauseGameplaySounds();
            m_play->scene.holdSounds();
            resetPlayInput();
            return;
        }
    }
    PlayScene::Inputs inputs =
        consoleOpen ? PlayScene::Inputs{} : readPlayInputs(deltaSeconds, sceneInput);
    if (const s32 flying = consolePlayer(); m_freeCamera.active() && flying >= 0) {
        inputs[static_cast<usize>(flying)] = PlayInput{}; // the keys fly the camera instead
    }
    // Noclip flies under the free camera too; the scene sets a flown seat's other controls
    // aside.
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        inputs[static_cast<usize>(player)].consoleFlight =
            consoleFlight(player, deltaSeconds, consoleOpen);
    }
    const bool wasLeaving = m_play->scene.leaving();
    const PlayScene::LocalStep stepped =
        m_play->scene.step(deltaSeconds, m_sessionInputs.advance(inputs), actions);
    for (const PlayScene::ConsoleReply& reply : stepped.replies) {
        m_console.print(reply.text);
    }
    // The scene now holds the console's cheats; read them back before any outcome closes it, so
    // they go with the party into the next scene.
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        m_consoleCheats[static_cast<usize>(seat)] = m_play->scene.cheats(seat);
    }
    const PlayOutcome outcome = stepped.outcome;
    if (outcome == PlayOutcome::Running) {
        if (auto* imported = m_play->world.imported(); imported && imported->takeShopRequest()) {
            if (openImportedShop(ShopVisit::Shop)) {
                return;
            }
        }
    }
    // Exit speech begins with the sinking spin, once for the party, and must
    // survive closing PlayScene before the tally. Temple/Underworld and the
    // boss wizard's departure have their own speeches.
    if (!wasLeaving && m_play->scene.leaving() && !m_play->world.imported() &&
        !m_play->world.isTower() && !m_play->world.ref().isSecret() &&
        m_play->world.ref().realmId != 5 && m_play->world.ref().realmId != 8 &&
        m_play->scene.victory().stage() <= BossVictory::Stage::Appearing) {
        m_exitSpeech.begin(m_options.unpackedDirectory, m_sounds.get(),
                           ExitRelics::remaining(m_play->world.placedItems()),
                           m_play->scene.party());
    }
    if (outcome == PlayOutcome::Travel) {
        if (m_play->scene.consoleTravelRequest()) {
            travelByConsole();
            return;
        }
        if (m_play->world.imported()) {
            finishLegendsTravel();
            return;
        }
        if (m_play->scene.secretTravel() && m_play->world.ref().isSecret() && m_parent) {
            Journey journey;
            journey.secret = true;
            journey.returningFromChallenge = true;
            journey.destination = m_parent->world.ref();
            journey.party = m_play->scene.party();
            if (m_play->scene.challenge().won()) {
                autoSave(journey.party);
            }
            m_play->scene.close();
            m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
            m_loadingPicture.cover();
            m_journey = std::move(journey);
            return;
        }
        // The party goes on with all it carries; back in the tower it arrives at the way in
        // of the realm it left.
        Journey journey;
        journey.secret = m_play->scene.secretTravel();
        journey.destination = m_play->scene.destination();
        journey.party = m_play->scene.party();
        journey.options.welcome = false;
        journey.options.arriving = true;
        journey.options.arrivalWorld = static_cast<u32>(std::max(m_play->world.ref().realmId, 0));
        const bool completedLevel = !journey.secret && !m_play->world.isTower();
        // A level left by its exit (or its boss's fall) is beaten by everyone still standing,
        // as game_main records it before the shop (towerRecordLevelBeaten); a party all
        // fallen records nothing.
        if (completedLevel) {
            const LevelRef& left = m_play->world.ref();
            const LevelInfo* level = m_play->world.level();
            recordLevelBeaten(journey.party, left.realmId, left.index,
                              level != nullptr ? level->rune : 0,
                              level != nullptr ? level->legend : 0);
            if (journey.destination.isTower()) {
                journey.completion = Journey::Completion{
                    LevelCompletion(level != nullptr ? level->bossType : -1,
                                    m_play->scene.victory().finished()),
                    m_play->scene.levelResults(),
                    level != nullptr ? level->shopMaxima : std::array<s32, 3>{1000, 100, 1000},
                    left.name};
            }
        }
        if (journey.secret && !m_play->world.ref().isSecret()) {
            m_play->scene.suspendForChallenge();
            m_parent = std::move(m_play);
            m_play = std::make_unique<PlaySession>();
        } else {
            m_play->scene.close();
        }
        m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
        m_loadingPicture.cover();
        m_journey = std::move(journey);
        return;
    }
    if (outcome == PlayOutcome::GameOver) {
        if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
            native && native->platform() == LegendsPlatform::N64) {
            // A failed visit still owns its permanent native discoveries. Keep
            // the normal abandoned-level gold/XP policy and AutoSave setting;
            // never turn defeat into stage completion or a GDL campaign grant.
            // GameOver is reached after every runtime has departed, so party()
            // intentionally contains nobody. Snapshot those actual participants
            // before the scene is destroyed, including their original save slots.
            std::vector<PartyMember> participants;
            for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
                if (const auto* runtime = m_play->scene.runtime(seat)) {
                    participants.push_back({seat, runtime->actor.save(), runtime->slot});
                }
            }
            auto party = m_play->scene.abandonedParty(participants);
            for (auto& member : party) {
                if (const auto* current = m_play->scene.liveSave(member.player)) {
                    retainLegendsProgress(member.save, *current, LegendsPlatform::N64);
                }
            }
            autoSave(party);
        }
        m_legendsOrigin.reset();
        m_legendsEntryParty.clear();
        m_legendsTowerParty.clear();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_parent.reset();
        // Normal GDL failure still does not overwrite manual saves.
        m_play->scene.close();
        m_play->world.clear();
        m_exitSpeech.close();
        m_attract.gameOver();
        setMaxFrameRate(m_config.display.maxFrameRate);
        startNextAttractScreen();
        return;
    }
    if (outcome == PlayOutcome::Leave) {
        m_legendsOrigin.reset();
        m_legendsEntryParty.clear();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        m_play->scene.close();
        log::info("Leaving the tower for the title screen");
        if (!startTitleScreen()) {
            startNextAttractScreen();
        }
    }
}

bool Gauntlet::saveSettings(const GameConfig& config) {
    return applySettings(config, true);
}
bool Gauntlet::applySettings(const GameConfig& config, bool persist) {
    const bool monitorChanged = config.display.monitor != m_config.display.monitor;
    const bool windowChanged =
        monitorChanged || config.display.windowMode != m_config.display.windowMode ||
        config.display.windowWidth != m_config.display.windowWidth ||
        config.display.windowHeight != m_config.display.windowHeight ||
        (!persist &&
         !config.display.matchesWindow(window().windowMode(), window().displayOptions().window));
    try {
        // Another language's tables take the place of the current ones, which every screen
        // reads through the same table; menus write their text again as they come back,
        // while a level's own messages change with the next level loaded.
        if (config.text.language != m_config.text.language) {
            StringTable next;
            if (!loadText(next, config.text.language)) {
                log::warn("No text tables for language '{}'", config.text.language);
                return false;
            }
            m_strings = std::move(next);
        }
        const bool presentationChanged = config.display.vsync != m_config.display.vsync ||
                                         config.display.sampleCount != m_config.display.sampleCount;
        const bool rateChanged =
            config.display.maxFrameRate != m_config.display.maxFrameRate ||
            config.timing.gameplayFrameRate != m_config.timing.gameplayFrameRate;
        const auto restoreWindow = [&] {
            if (monitorChanged) {
                window().selectMonitor(m_config.display.monitor);
            }
            window().setDisplayMode(m_config.display.windowMode,
                                    {m_config.display.windowWidth, m_config.display.windowHeight});
        };
        if (windowChanged) {
            renderDevice().waitIdle();
            if (monitorChanged && !window().selectMonitor(config.display.monitor)) {
                return false;
            }
            const Extent2D size{config.display.windowWidth, config.display.windowHeight};
            // Another display may lack the mode chosen on this one: its desktop stands in.
            if (!window().setDisplayMode(config.display.windowMode, size) &&
                (!monitorChanged || !window().setDisplayMode(config.display.windowMode,
                                                             window().displayOptions().desktop))) {
                restoreWindow();
                return false;
            }
        }
        try {
            if (persist) {
                config.saveFile(GameConfig::userSettingsPath());
            }
        } catch (...) {
            if (windowChanged) {
                restoreWindow();
            }
            throw;
        }
        window().stopRumble();
        m_config = config;
        m_sounds->setMasterVolume(config.audio.masterVolume);
        m_sounds->setCategoryVolume(SoundCategory::Music, config.audio.musicVolume);
        m_sounds->setCategoryVolume(SoundCategory::Effects, config.audio.effectsVolume);
        m_sounds->setCategoryVolume(SoundCategory::Movie, config.audio.movieVolume);
        m_movie.setVolume(config.audio.masterVolume * config.audio.movieVolume);
        m_audio->mixer().setStereo(config.audio.stereo);
        if (presentationChanged) {
            renderDevice().setPresentation(config.display.vsync, config.display.sampleCount);
        }
        renderDevice().setTextureFiltering(config.display.textureFiltering);
        renderDevice().setSmoothSprites(config.display.smoothSprites);
        renderDevice().setAuthoredSilhouettes(config.display.authoredSilhouettes);
        if (rateChanged) {
            setMaxFrameRate(m_play->scene.isOpen() || m_demo.isOpen()
                                ? config.timing.gameplayFrameRate
                                : config.display.maxFrameRate);
        }
        resetPlayInput();
        return true;
    } catch (const std::exception& e) {
        log::warn("Could not save settings: {}", e.what());
        return false;
    }
}

void Gauntlet::updatePause(f64 deltaSeconds, const SceneInput& sceneInput) {
    const s32 pausePlayer = m_pause.player();
    const auto outcome = m_pause.update(deltaSeconds, sceneInput.pause(m_pause.player()));
    if (outcome == PauseOutcome::Running) {
        return;
    }
    auto party = m_pause.party();
    m_pause.close();
    m_audio->mixer().setPaused(false);
    if (m_play) {
        m_play->scene.releaseSounds();
    }
    resetPlayInput();
    if (outcome == PauseOutcome::Resume) {
        return;
    }
    if (outcome == PauseOutcome::Desktop) {
        requestQuit();
        return;
    }
    if (outcome == PauseOutcome::Manage) {
        if (startPlayerSelect(pausePlayer, party, true)) {
            m_play->scene.close();
        }
        return;
    }
    if (outcome == PauseOutcome::Title) {
        // Return to Main Menu leaves for the title screen at once. A level left is given up
        // as Quit Level gives it up; the party's autosave keeps what it brought out.
        m_legendsOrigin.reset();
        m_legendsEntryParty.clear();
        m_legendsTowerParty.clear();
        if (m_legendsSounds) {
            m_legendsSounds->stopAll();
            m_legendsSounds.reset();
        }
        if (!m_play->world.isTower()) {
            party = m_play->scene.abandonedParty(party);
            if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
                native && native->platform() == LegendsPlatform::N64) {
                for (auto& member : party) {
                    if (const auto* current = m_play->scene.liveSave(member.player)) {
                        retainLegendsProgress(member.save, *current, LegendsPlatform::N64);
                    }
                }
            }
        }
        autoSave(party);
        m_parent.reset();
        m_play->scene.close();
        m_play->world.clear();
        m_exitSpeech.close();
        setMaxFrameRate(m_config.display.maxFrameRate);
        log::info("Leaving for the main menu");
        if (!startTitleScreen()) {
            startNextAttractScreen();
        }
        return;
    }
    if (outcome == PauseOutcome::Shop || outcome == PauseOutcome::Inventory) {
        if (m_play->world.imported()) {
            openImportedShop(outcome == PauseOutcome::Shop ? ShopVisit::Shop
                                                           : ShopVisit::Inventory);
            return;
        }
        Journey journey;
        journey.destination = LevelRef::tower();
        journey.party = party;
        journey.options.welcome = false;
        if (const auto* actor = m_play->scene.actor(pausePlayer)) {
            journey.options.position = actor->position();
        }
        if (m_afterLevel.open(
                renderDevice(), services(), party, {}, {1000, 100, 1000}, m_play->world.ref().name,
                outcome == PauseOutcome::Shop ? ShopVisit::Shop : ShopVisit::Inventory)) {
            m_play->scene.close();
            m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
            m_loadingPicture.cover();
            m_journey = std::move(journey);
        }
        return;
    }
    if (outcome == PauseOutcome::ReturnTower) {
        if (m_legendsOrigin) {
            quitLegendsLevel();
            return;
        }
        // Quitting a level gives up what it gave, as dying in it does: the original kills
        // the whole party (opt_restart_request), and init_shop with nobody standing skips
        // the tally and the shop for the tower, so neither is shown here.
        party = m_play->scene.abandonedParty(party);
        Journey journey;
        journey.destination = LevelRef::tower();
        journey.party = party;
        journey.options.welcome = false;
        journey.options.arriving = true;
        journey.options.arrivalWorld = static_cast<u32>(std::max(m_play->world.ref().realmId, 0));
        autoSave(party);
        m_play->scene.close();
        m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
        m_loadingPicture.cover();
        m_journey = std::move(journey);
    }
}

bool Gauntlet::openImportedShop(ShopVisit visit) {
    if (!m_play->scene.isOpen() || !m_play->world.imported() || m_journey ||
        (visit != ShopVisit::Shop && visit != ShopVisit::Inventory)) {
        return false;
    }
    Journey journey;
    journey.destination = m_play->world.ref();
    journey.party = m_play->scene.party();
    journey.resumeImported = true;
    if (!m_afterLevel.open(renderDevice(), services(), journey.party, {}, {1000, 100, 1000},
                           m_play->world.ref().name, visit)) {
        return false;
    }
    // Menu voices use the same mixer, so suspend only the scene's own handles.
    // Its camera, enemies and fixtures remain at precisely this tick.
    m_play->scene.suspendForImportedLevel();
    if (m_legendsSounds) {
        m_legendsSounds->setCategoryPaused(SoundCategory::Music, true);
    }
    m_journey = std::move(journey);
    resetPlayInput();
    return true;
}

void Gauntlet::updateAfterLevel(f64 deltaSeconds, const SceneInput& sceneInput) {
    static_assert(ShopSession::kSeats == kInputSlots, "offline, seat n reads input slot n");
    ShopSession::Inputs inputs;
    for (s32 player = 0; player < static_cast<s32>(kInputSlots); ++player) {
        inputs[static_cast<usize>(player)] = sceneInput.slot(player);
    }
    if (m_afterLevel.update(deltaSeconds, inputs)) {
        GDL_VERIFY(m_journey.has_value(), "Shop requires a pending journey");
        if (m_journey->resumeImported) {
            auto party = m_afterLevel.session().party();
            if (commitImportedShop(m_play->scene, m_journey->party, party)) {
                if (m_afterLevel.session().visit() == ShopVisit::Shop) {
                    autoSave(party);
                }
                for (const auto& member : party) {
                    m_play->scene.setSaveSlot(member.player, member.slot);
                }
            } else {
                log::warn("Imported shop return refused: the parked party changed");
            }
            m_afterLevel.close();
            m_journey.reset();
            m_play->scene.resumeAfterImportedLevel();
            if (m_legendsSounds) {
                m_legendsSounds->setCategoryPaused(SoundCategory::Music, false);
            }
            resetPlayInput();
            return;
        }
        m_journey->party = m_afterLevel.session().party();
        const bool manageCharacters = m_afterLevel.session().visit() == ShopVisit::Level ||
                                      m_afterLevel.session().visit() == ShopVisit::Completion;
        if (m_afterLevel.session().visit() == ShopVisit::Level ||
            m_afterLevel.session().visit() == ShopVisit::Shop) {
            autoSave(m_journey->party);
        }
        m_afterLevel.close();
        if (m_journey->completion) {
            m_journey->completion->flow.advance();
            return;
        }
        if (!manageCharacters) {
            // Tower Shop and Inventory are menus, not another level entry.
            // Restore play without a map, entering speech or encounter movie.
            finishJourney();
            return;
        }
        if (!m_select.openAfterLevel(renderDevice(), services(), m_journey->party,
                                     &m_play->world)) {
            log::warn("Post-level character menu unavailable; keeping rewards and returning.");
        }
    }
}

bool Gauntlet::updateCompletion(f64 deltaSeconds) {
    GDL_VERIFY(m_journey.has_value(), "Completion requires a pending journey");
    Journey& journey = *m_journey;
    if (!journey.completion) {
        return false;
    }
    auto& completion = *journey.completion;
    switch (completion.flow.stage()) {
    case LevelCompletion::Stage::Movie:
        if (!journey.movieStarted) {
            journey.movieStarted = startMovie(completion.flow.movie());
            if (journey.movieStarted) {
                log::info("Boss completion movie: {}", completion.flow.movie());
                return true;
            }
        } else if (m_movie.update(deltaSeconds)) {
            // do_gamemovie permits the action-button skip only for movie_state 1;
            // these two ending movies use state 2 and run through their ending.
            return true;
        }
        m_movie.close();
        m_movieActive = false;
        journey.movieStarted = false;
        completion.flow.advance();
        return true;
    case LevelCompletion::Stage::Results:
        if (!m_afterLevel.open(renderDevice(), services(), journey.party, completion.results,
                               completion.maxima, completion.levelName, completion.flow.visit())) {
            log::warn("Completion results unavailable; preserving rewards and offering save.");
            completion.flow.advance();
        }
        return true;
    case LevelCompletion::Stage::Characters:
        if (!m_select.openAfterLevel(renderDevice(), services(), journey.party, &m_play->world)) {
            log::warn("Post-level character menu unavailable; keeping rewards and returning.");
            completion.flow.advance();
        }
        return true;
    case LevelCompletion::Stage::Done: return false;
    }
    return false;
}

void Gauntlet::updateJourney(f64 deltaSeconds, const SceneInput& sceneInput) {
    GDL_VERIFY(m_journey.has_value(), "Updating a journey requires a pending journey");
    Journey& journey = *m_journey;
    if (!journey.shown) {
        return;
    }
    if (updateCompletion(deltaSeconds)) {
        return;
    }
    if (journey.importedDestination) {
        // The native destination supplies its own entry camera and title.
        // Never ask the GameCube level catalog to interpret its map identity.
        finishJourney();
        return;
    }
    if (!journey.presentationStarted) {
        journey.presentationStarted = true;
        m_levelLoading.open(renderDevice(), services(), journey.destination, journey.party,
                            journey.secret);
        journey.movie = m_levelLoading.movie();
        // Present at least one frame of the map before advancing its clock.
        return;
    }
    if (journey.movieStarted) {
        bool skip = false;
        for (const auto& member : journey.party) {
            const auto menu = sceneInput.movie(member.player);
            skip = skip || menu.start;
        }
        if (!skip && m_movie.update(deltaSeconds)) {
            return;
        }
        m_movie.close();
        m_movieActive = false;
        finishJourney();
        return;
    }
    if (!m_levelLoading.update(static_cast<f32>(deltaSeconds))) {
        return;
    }
    if (LevelLoadingScreen::movieWanted(journey.movie, journey.party) &&
        startMovie(journey.movie)) {
        journey.movieStarted = true;
        m_levelLoading.close();
        return;
    }
    finishJourney();
}

void Gauntlet::finishJourney() {
    GDL_VERIFY(m_journey.has_value(), "Finishing a journey requires a pending journey");
    Journey journey = std::move(*m_journey);
    m_journey.reset();
    m_loadingPicture.release();
    m_levelLoading.close();
    if (journey.importedDestination) {
        const auto& destination = *journey.importedDestination;
        const auto* previous = m_play.get();
        const auto generation = m_play->scene.loadGeneration();
        consoleLegendsOpen(m_console, destination.platform, destination.directory, true, true,
                           journey.party, true);
        if (m_play.get() != previous || m_play->scene.loadGeneration() != generation)
            return;
        // Keep the purchased inventory and save-slot choices if loading failed.
        // Done retries the same destination; Save/Load/Quit remain available.
        // Neither the tally nor completion grants run for a second time.
        m_console.print("Native destination unavailable. Your completed party and shop purchases "
                        "were kept; save or choose Done to retry.");
        m_journey = std::move(journey);
        m_loadingPicture.cover();
        if (!m_select.openAfterLevel(renderDevice(), services(), m_journey->party, &m_play->world))
            log::warn("Native travel recovery menu unavailable; retaining the pending party.");
        resetPlayInput();
        return;
    }
    if (journey.returningFromChallenge && m_parent) {
        returnFromChallenge(journey.party);
        return;
    }
    if (startLevel(journey.destination, journey.party, journey.options)) {
        return;
    }
    if (journey.secret && m_parent) {
        returnFromChallenge(journey.party);
        return;
    }
    if (!startLevel(LevelRef::tower(), journey.party, journey.options) && !startTitleScreen()) {
        startNextAttractScreen();
    }
}

void Gauntlet::travelByConsole() {
    const auto& request = m_play->scene.consoleTravelRequest();
    GDL_VERIFY(request.has_value(), "Console travel needs the stepped scene's request");
    const std::optional<LevelRef> parent =
        m_parent ? std::optional{m_parent->world.ref()} : std::nullopt;
    const ConsoleJourney plan = consoleJourney(*request, m_play->world.ref(), parent);
    m_freeCamera.stop(); // its place means nothing in another level
    if (plan.kind == ConsoleTravelKind::ReturnToParent) {
        // Nothing is won or recorded by the console: the parent takes up where it waited, and
        // the challenge that asked goes with its session. Its last frame may still be drawing:
        // direct render backends do not retain texture leases for queued frames.
        const std::vector<PartyMember> party = m_play->scene.party();
        renderDevice().waitIdle();
        m_play->scene.close();
        returnFromChallenge(party);
        if (plan.options.position) {
            m_play->scene.relocate(*plan.options.position);
        }
        return;
    }
    Journey journey;
    journey.destination = request->destination;
    journey.party = m_play->scene.party();
    journey.options = plan.options;
    journey.secret = plan.secret;
    if (plan.kind == ConsoleTravelKind::EnterChallenge) {
        m_play->scene.suspendForChallenge();
        m_parent = std::move(m_play);
        m_play = std::make_unique<PlaySession>();
    } else {
        m_play->scene.close();
    }
    // A travel request left in an open scene would end every tick after.
    GDL_VERIFY(!m_play->scene.isOpen(), "Console travel left the scene that asked for it open");
    m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
    m_loadingPicture.cover();
    m_journey = std::move(journey);
}

void Gauntlet::returnFromChallenge(std::span<const PartyMember> party) {
    m_play.reset();
    m_play = std::move(m_parent);
    m_play->scene.resumeFromChallenge(party);
    // The parent held the cheats it had when the challenge began; the challenge's go on.
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        m_play->scene.setCheats(seat, m_consoleCheats[static_cast<usize>(seat)]);
    }
    log::info("Returning from secret challenge to {}", m_play->world.ref().name);
    resetPlayInput();
}

void Gauntlet::onRender(RenderDevice& device) {
    // The console's suggestion is drawn by what the session admits now.
    m_console.setSession(consoleSession());
    renderScene(device);
    if (m_online && m_online->consoleAllowed() && m_console.visible()) {
        const auto width = static_cast<f32>(m_config.display.frameWidth);
        const auto height = static_cast<f32>(m_config.display.frameHeight);
        const auto extent = device.framebufferExtent();
        const auto projection = makeLetterboxProjection(
            width, height, static_cast<f32>(extent.width), static_cast<f32>(extent.height));
        m_canvas.begin(device,
                       makeVirtualScreenTransform(
                           projection, static_cast<f32>(m_config.display.virtualWidth),
                           static_cast<f32>(m_config.display.virtualHeight), width, height));
        m_console.draw(m_canvas);
        m_canvas.end();
    }
    // One final pass covers movies, loading, menus, play, pause and the idle screen.
    if (!m_hudHidden) {
        m_buildLabel.render(device, device.framebufferExtent());
    }
    if (m_levelDebug.visible() && consoleScene() != nullptr) {
        m_levelDebug.render(device, device.framebufferExtent(), levelDebugLines());
    }
    if (!m_hudHidden) {
        m_autoSaveIcon.render(device, device.framebufferExtent());
    }
}

std::vector<std::string> Gauntlet::levelDebugLines() const {
    const auto* scene = consoleScene();
    const auto* world = consoleWorld();
    if (scene == nullptr || world == nullptr) {
        return {};
    }
    s32 enemiesAlive = 0;
    const Enemies& enemies = scene->opponents().enemies();
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        enemiesAlive += enemies.alive(id) ? 1 : 0;
    }
    return LevelDebugOverlay::describe(*world, scene->participants(), scene->camera().localState(),
                                       enemiesAlive);
}

PlayScene::Inputs Gauntlet::readPlayInputs(f64 deltaSeconds, const SceneInput& sceneInput) {
    if (!m_online) {
        return m_playInput.read(sceneInput, deltaSeconds, OfflineAim{m_play->scene});
    }
    PlayScene::Inputs inputs = m_playInput.read(sceneInput, deltaSeconds, OnlineAim{*m_online});
    for (s32 slot = 0; slot < PlayScene::kPlayerCount; ++slot) {
        inputs[static_cast<usize>(slot)].consoleFlight = consoleFlight(slot, deltaSeconds, false);
    }
    return inputs;
}

void Gauntlet::renderScene(RenderDevice& device) {
    const Extent2D framebuffer = device.framebufferExtent();
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const Mat4 projection =
        makeLetterboxProjection(frameWidth, frameHeight, static_cast<f32>(framebuffer.width),
                                static_cast<f32>(framebuffer.height));
    if (m_legendsPreview.isOpen()) {
        m_legendsPreview.render(device, projection, frameWidth, frameHeight,
                                glm::radians(m_config.camera.horizontalFovDegrees));
        if (m_console.visible()) {
            m_canvas.begin(device, makeVirtualScreenTransform(
                                       projection, static_cast<f32>(m_config.display.virtualWidth),
                                       static_cast<f32>(m_config.display.virtualHeight), frameWidth,
                                       frameHeight));
            m_console.draw(m_canvas);
            m_canvas.end();
        }
        return;
    }
    if (m_netplay) {
        if (m_select.isOpen()) {
            m_select.render(device, projection, frameWidth, frameHeight);
        } else {
            if (m_online && m_online->playing()) {
                m_online->render(device, projection, frameWidth, frameHeight, renderDeltaSeconds(),
                                 presentationAlpha());
            } else {
                m_title.render(device, projection, frameWidth, frameHeight);
            }
            if (m_netplay->overlay) {
                m_netplay->menu.render(device, projection, frameWidth, frameHeight);
            }
        }
        return;
    }
    if (m_online) {
        m_online->render(device, projection, frameWidth, frameHeight, renderDeltaSeconds(),
                         presentationAlpha());
        return;
    }
    if (m_idleScreen.isOpen()) {
        m_idleScreen.render(device, projection, frameWidth, frameHeight,
                            glm::radians(m_config.camera.horizontalFovDegrees),
                            presentationAlpha());
        return;
    }
    if (m_demo.isOpen()) {
        m_demo.render(device, projection, frameWidth, frameHeight, presentationAlpha());
        return;
    }
    if (m_afterLevel.isOpen()) {
        m_afterLevel.render(device, projection, frameWidth, frameHeight);
        return;
    }
    if (m_select.isOpen()) {
        m_select.render(device, projection, frameWidth, frameHeight);
        return;
    }
    if (m_journey.has_value()) {
        if (m_journey->movieStarted) {
            m_movie.render(device, projection, Rect{0, 0, frameWidth, frameHeight});
            return;
        }
        const auto width =
            m_levelLoading.active() ? 512.0f : static_cast<f32>(m_config.display.virtualWidth);
        const auto height =
            m_levelLoading.active() ? 384.0f : static_cast<f32>(m_config.display.virtualHeight);
        m_canvas.begin(
            device, makeVirtualScreenTransform(projection, width, height, frameWidth, frameHeight));
        if (m_levelLoading.active()) {
            m_levelLoading.draw(m_canvas, device);
        } else {
            m_loadingPicture.draw(m_canvas, width, height, TransitionScreen::Area::FullScreen);
        }
        m_canvas.end();
        m_journey->shown = true;
        return;
    }
    if (m_movieActive) {
        m_movie.render(device, projection, Rect{0.0f, 0.0f, frameWidth, frameHeight});
        return;
    }
    if (m_title.isOpen()) {
        m_title.render(device, projection, frameWidth, frameHeight);
        return;
    }
    if (m_play->scene.isOpen()) {
        // Texture transfers must precede all drawing, including the world behind
        // an overlay. The acid dismissal changes the pause parchment each frame.
        m_pause.prepare(device);
        m_play->scene.render(device, projection, frameWidth, frameHeight, m_pause.isOpen(),
                             presentationAlpha());
        if (m_pause.isOpen()) {
            m_pause.render(device, projection, frameWidth, frameHeight);
        }
        if (m_console.visible()) {
            // Over everything, in the status boxes' own pixel space so it spans their strip.
            m_canvas.begin(device, makeVirtualScreenTransform(
                                       projection, static_cast<f32>(m_config.display.virtualWidth),
                                       static_cast<f32>(m_config.display.virtualHeight), frameWidth,
                                       frameHeight));
            m_console.draw(m_canvas);
            m_canvas.end();
        }
        return;
    }
    m_smokeTest.render(device, projection, static_cast<f32>(clock().totalSeconds()));
}

void Gauntlet::autoSave(std::span<PartyMember> party) {
    if (m_config.game.autoSave) {
        saveLocalParty(party);
    }
}

bool Gauntlet::saveLocalParty(std::span<PartyMember> party) {
    if (party.empty() || !m_saves.open(m_config.saveDirectory(), m_config.save.slots)) {
        return false;
    }
    const auto result = autoSaveParty(m_saves, party);
    if (result.written > 0) {
        m_autoSaveIcon.show();
    }
    if (result.failed > 0 || result.unavailable > 0) {
        log::warn("Local save: {} saved, {} failed, {} without an available slot", result.written,
                  result.failed, result.unavailable);
    }
    return result.written == party.size() && result.failed == 0 && result.unavailable == 0;
}

void Gauntlet::onShutdown() {
    m_netplay.reset();
    window().stopRumble();
    m_online.reset();
    const bool completingNative = m_journey && m_journey->importedDestination;
    if (completingNative) {
        auto party = legendsCompletionParty(m_journey->party, m_afterLevel, m_select);
        autoSave(party);
    } else if (m_play && m_play->scene.isOpen()) {
        if (const auto* native = dynamic_cast<const LegendsLevel*>(m_play->world.imported());
            native && native->platform() == LegendsPlatform::N64) {
            // Closing the window during the GameOver caption precedes its
            // outcome callback. Include departed participants just as that
            // callback does, or their permanent native discoveries disappear.
            std::vector<PartyMember> participants;
            for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
                if (const auto* runtime = m_play->scene.runtime(seat)) {
                    participants.push_back({seat, runtime->actor.save(), runtime->slot});
                }
            }
            auto party = m_play->scene.abandonedParty(participants);
            for (auto& member : party) {
                if (const auto* current = m_play->scene.liveSave(member.player)) {
                    retainLegendsProgress(member.save, *current, LegendsPlatform::N64);
                }
            }
            autoSave(party);
        }
    }
    if (!completingNative)
        commitLegendsProgress();
    m_legendsPreview.close();
    if (m_legendsSounds) {
        m_legendsSounds->stopAll();
        m_legendsSounds.reset();
    }
    m_buildLabel.release();
    m_levelDebug.release();
    m_console.release();
    m_autoSaveIcon.release();
    m_idleScreen.close();
    m_demo.close();
    m_parent.reset();
    m_legendsOrigin.reset();
    m_legendsEntryParty.clear();
    m_legendsTowerParty.clear();
    m_pause.close();
    m_movie.close();
    m_title.close();
    m_select.close();
    m_play->scene.close();
    m_afterLevel.close();
    m_loadingPicture.release();
    m_exitSpeech.close();
    m_levelLoading.close();
    m_journey.reset();
    m_play->world.clear();
    m_smokeTest.shutdown();
    // Every screen still holding the view finds no sound, movie mixer or files from here on.
    m_services.assets = nullptr;
    m_services.sounds = nullptr;
    m_services.movieMixer = nullptr;
    m_assets.reset();
    m_sounds.reset();
    m_audio.reset();
    m_windowLive = false;
}

bool Gauntlet::startMovie(std::string_view name) {
    m_movie.setVolume(m_config.audio.masterVolume * m_config.audio.movieVolume);
    const auto file = m_assets->find(std::format("{}/{}.avi", kMovieDirectory, name));
    if (!file.has_value()) {
        log::warn("Movie '{}' not found under {}", name, paths::utf8(m_assets->root()));
        return false;
    }
    if (!m_movie.open(renderDevice(), m_audio->mixer(), *file)) {
        return false;
    }
    m_movieActive = true;
    return true;
}

bool Gauntlet::startTitleScreen() {
    m_parent.reset();
    // The title begins a new game: no console cheat, noclip flight or free camera lasts past it.
    m_consoleCheats = {};
    resetConsoleFlights();
    m_freeCamera.stop();
    m_exitSpeech.close();
    setMaxFrameRate(m_config.display.maxFrameRate);
    if (m_title.open(renderDevice(), services(), &m_liveSettings)) {
        m_attract.titleShown();
        return true;
    }
    if (!m_titleWarned) {
        m_titleWarned = true;
        log::warn("Title screen unavailable; unpack the game data into {} with gdlunpack",
                  paths::utf8(m_options.unpackedDirectory));
    }
    return false;
}

void Gauntlet::startNextAttractScreen() {
    for (usize attempts = 0; attempts < AttractSequencer::kScreenTable.size(); ++attempts) {
        const AttractStep step = m_attract.next();
        if (step.screen == AttractScreen::Flyby || step.screen == AttractScreen::Demo) {
            if (m_demo.openNext(renderDevice(), services())) {
                setMaxFrameRate(m_config.timing.gameplayFrameRate);
                return;
            }
            continue;
        }
        if (step.screen == AttractScreen::TitleScreen) {
            if (startTitleScreen()) {
                return;
            }
            continue;
        }
        if (step.movie.empty()) {
            continue;
        }
        if (startMovie(step.movie)) {
            return;
        }
    }
    log::warn("No attract screen could be shown; showing the smoke test scene");
}

} // namespace gdl::game
