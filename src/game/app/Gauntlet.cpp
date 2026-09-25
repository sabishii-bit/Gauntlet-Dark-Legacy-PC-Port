#include "game/app/Gauntlet.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/PngImage.h"
#include "engine/assets/WorldData.h"
#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"

#include "game/app/Scenario.h"
#include "game/menu/MenuInput.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

namespace {

constexpr std::string_view kMovieDirectory = "VQMOVIES";
constexpr std::string_view kTextDirectory = "text";
constexpr f64 kFpsReportInterval = 2.0;
constexpr std::string_view kWindowIcon = "carddemo/icon0.png"; ///< unpacked memory-card icon

} // namespace

Gauntlet::Gauntlet(ApplicationDesc desc, GameOptions options, GameConfig config)
    : Application(std::move(desc)), m_options(std::move(options)), m_config(std::move(config)) {}

/** The memory-card icon as the window's icon, when the card art has been unpacked. */
void Gauntlet::applyWindowIcon() {
    const std::filesystem::path file = m_options.unpackedDirectory / kWindowIcon;
    if (!std::filesystem::exists(file)) {
        return;
    }
    try {
        const Image icon = loadImageFile(file);
        window().setIcon(std::span<const Image>(&icon, 1));
    } catch (const std::exception& e) {
        log::warn("Window icon {} unusable: {}", file.string(), e.what());
    }
}

void Gauntlet::onInit() {
    applyWindowIcon();
    if (!m_strings.load(m_options.dataDirectory / kTextDirectory, m_config.text.language)) {
        log::warn("No text tables under {}; identifiers will show instead of text",
                  (m_options.dataDirectory / kTextDirectory).string());
    }
    m_audio = std::make_unique<AudioDevice>();
    m_audio->mixer().setStereo(m_config.audio.stereo);
    m_sounds = std::make_unique<SoundPlayer>(m_audio->mixer());
    m_sounds->setMasterVolume(m_config.audio.masterVolume);
    m_sounds->setCategoryVolume(SoundCategory::Music, m_config.audio.musicVolume);
    m_sounds->setCategoryVolume(SoundCategory::Effects, m_config.audio.effectsVolume);
    m_assets = std::make_unique<AssetLocator>(assetDirectory());
    m_smokeTest.init(renderDevice());

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
        log::info("Scenario {}: {} in the party", file.string(), scenario.party.size());
        if (scenario.afterLevel) {
            const auto sceneContext = context();
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
            if (!m_afterLevel.open(renderDevice(), sceneContext, journey.party, scenario.results,
                                   level->shopMaxima, levelName)) {
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
                log::error("Scenario {}: no level named {}", file.string(), scenario.level);
                return false;
            }
            return startLevel(*level, scenario.partyMembers(), scenario.tower);
        }
        return startTower(scenario.partyMembers(), scenario.tower);
    } catch (const std::exception& e) {
        log::error("Scenario {}: {}", file.string(), e.what());
        return false;
    }
}

GameContext Gauntlet::context() {
    GameContext context;
    context.config = &m_config;
    context.strings = &m_strings;
    context.sounds = m_sounds.get();
    context.assets = m_assets.get();
    context.tower = &m_towerWorld;
    if (!m_levels.loaded()) {
        m_levels.load(m_options.unpackedDirectory);
    }
    context.levels = &m_levels;
    context.unpackedRoot = m_options.unpackedDirectory;
    context.saveSettings = [this](const GameConfig& config) { return saveSettings(config); };
    context.previewAudio = [this](const AudioConfig& audio) {
        m_sounds->setCategoryVolume(SoundCategory::Music, audio.musicVolume);
        m_sounds->setCategoryVolume(SoundCategory::Effects, audio.effectsVolume);
        m_audio->mixer().setStereo(audio.stereo);
    };
    return context;
}

void Gauntlet::onUpdate(f64 deltaSeconds) {
    if (updateIdle(deltaSeconds)) {
        return;
    }
    // Gameplay and editable menus own Escape; only passive screens treat it as quit.
    if (readMenuInput(input(), m_config.menu).escape && !m_tower.isOpen() && !m_pause.isOpen() &&
        !m_journey.has_value() && !(m_title.isOpen() && m_title.optionsOpen()) &&
        !(m_select.isOpen() && m_select.typing())) {
        requestQuit();
    }

    if (m_pause.isOpen()) {
        updatePause(deltaSeconds);
    } else if (m_afterLevel.isOpen()) {
        updateAfterLevel(deltaSeconds);
    } else if (m_journey.has_value()) {
        updateJourney(deltaSeconds);
    } else if (m_demo.isOpen()) {
        updateAttract(deltaSeconds);
    } else if (m_movieActive) {
        updateMovie(deltaSeconds);
    } else if (m_title.isOpen()) {
        updateTitle(deltaSeconds);
    } else if (m_select.isOpen()) {
        updateSelect(deltaSeconds);
    } else if (m_tower.isOpen()) {
        updateTower(deltaSeconds);
    }
    m_sounds->update();

    m_fpsAccumulator += deltaSeconds;
    ++m_fpsFrames;
    if (m_fpsAccumulator >= kFpsReportInterval) {
        log::trace("{:.1f} fps", static_cast<f64>(m_fpsFrames) / m_fpsAccumulator);
        m_fpsAccumulator = 0.0;
        m_fpsFrames = 0;
    }
}

bool Gauntlet::updateIdle(f64 deltaSeconds) {
    const bool eligible =
        m_options.previewScreensaver ||
        (!m_movieActive && !m_journey.has_value() && !m_demo.isOpen() &&
         (!m_title.isOpen() || m_title.menuOpen() || m_title.optionsOpen()) &&
         !m_tower.gameOver().active() &&
         (m_tower.isOpen() || m_select.isOpen() || m_afterLevel.isOpen() || m_title.isOpen()));
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
        for (auto& controls : m_controls) {
            controls.reset();
        }
        if (m_options.previewScreensaver) {
            requestQuit();
        }
    }
    return wasOpen || m_idleWatch.consumingInput();
}

void Gauntlet::updateAttract(f64 deltaSeconds) {
    const auto outcome = m_demo.update(deltaSeconds, readMenuInput(input(), m_config.menu));
    if (outcome == AttractOutcome::Running) {
        return;
    }
    m_demo.close();
    if (outcome != AttractOutcome::Title || !startTitleScreen()) {
        startNextAttractScreen();
    }
}

void Gauntlet::updateMovie(f64 deltaSeconds) {
    const MenuInput menu = readMenuInput(input(), m_config.menu);
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

void Gauntlet::updateTitle(f64 deltaSeconds) {
    const TitleOutcome outcome =
        m_title.update(deltaSeconds, readMenuInput(input(), m_config.menu));
    if (outcome == TitleOutcome::Running) {
        return;
    }
    m_title.close();
    if (outcome == TitleOutcome::StartGame && startPlayerSelect(playerPressingStart())) {
        return;
    }
    startNextAttractScreen();
}

/** The player whose Start or Select is down this frame; the first when none is. */
s32 Gauntlet::playerPressingStart() const {
    for (s32 player = 0; player < PlayerSelectScene::kLaneCount; ++player) {
        const MenuInput menu =
            readMenuInput(input(), m_config.menu, MenuInputSource::forPlayer(player));
        if (menu.start || menu.select) {
            return player;
        }
    }
    return 0;
}

bool Gauntlet::startPlayerSelect(s32 startingPlayer) {
    if (m_select.open(renderDevice(), context(), startingPlayer)) {
        return true;
    }
    log::warn("Player select unavailable; unpack the game data into {} with gdlunpack",
              m_options.unpackedDirectory.string());
    return false;
}

void Gauntlet::updateSelect(f64 deltaSeconds) {
    PlayerSelectScene::Inputs inputs;
    for (s32 player = 0; player < PlayerSelectScene::kLaneCount; ++player) {
        inputs[static_cast<usize>(player)] =
            readMenuInput(input(), m_config.menu, m_select.inputSource(player));
    }
    const SelectOutcome outcome = m_select.update(deltaSeconds, inputs);
    if (outcome == SelectOutcome::Running) {
        return;
    }
    std::vector<PartyMember> party;
    for (s32 player = 0; player < PlayerSelectScene::kLaneCount; ++player) {
        const SelectLane& lane = m_select.lane(player);
        if (lane.lockedIn()) {
            party.push_back(PartyMember{player, lane.save(), lane.slotInUse()});
        }
    }
    m_select.close();
    if (outcome == SelectOutcome::Done && startTower(party)) {
        return;
    }
    if (!startTitleScreen()) {
        startNextAttractScreen();
    }
}

bool Gauntlet::startLevel(const LevelRef& level, std::span<const PartyMember> party,
                          const PlayOptions& options) {
    if (party.empty()) {
        return false;
    }
    if (!m_towerWorld.built() || !(m_towerWorld.ref() == level)) {
        if (!m_towerWorld.load(renderDevice(), m_options.unpackedDirectory, level)) {
            log::warn("Level {} is unavailable; unpack it with gdlunpack --only", level.name);
            return false;
        }
    }
    if (m_tower.open(renderDevice(), context(), m_towerWorld, party, options)) {
        for (auto& controls : m_controls) {
            controls.reset();
        }
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
    if (m_towerWorld.built() && !m_towerWorld.isTower()) {
        return startLevel(LevelRef::tower(), party, options);
    }
    if (m_tower.open(renderDevice(), context(), m_towerWorld, party, options)) {
        for (auto& controls : m_controls) {
            controls.reset();
        }
        log::info("Every player is ready; entering the tower");
        setMaxFrameRate(m_config.timing.gameplayFrameRate);
        return true;
    }
    log::warn("The tower is unavailable; unpack the levels with gdlunpack --levels");
    return false;
}

void Gauntlet::updateTower(f64 deltaSeconds) {
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (!m_tower.canPause(player)) {
            continue;
        }
        const auto menu = readMenuInput(input(), m_config.menu, MenuInputSource::forPlayer(player));
        if (!m_tower.leaving() && ((menu.start && !menu.select) || menu.escape) &&
            m_pause.open(renderDevice(), context(), m_tower.party(), player)) {
            // Music continues in menus, including while adjusting its volume.
            for (auto& controls : m_controls) {
                controls.reset();
            }
            return;
        }
    }
    PlayScene::Inputs inputs;
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        const MenuInputSource source = MenuInputSource::forPlayer(player);
        PlayInput& in = inputs[static_cast<usize>(player)];
        in.move = readMoveInput(input(), m_config.play, source.keyboard, source.pad);
        const PlayButtons buttons = m_controls[static_cast<usize>(player)].read(
            input(), m_config.play, source.keyboard, source.pad, static_cast<f32>(deltaSeconds));
        in.attack = buttons.attack;
        in.usePotion = buttons.usePotion;
        in.throwPotion = buttons.throwPotion;
        in.shieldPotion = buttons.shieldPotion;
        in.strafe = buttons.strafe;
        in.strongAttack = buttons.strongAttack;
        in.turbo = buttons.turbo;
        in.chargePressed = buttons.chargePressed;
        in.attackPressed = buttons.attackPressed;
        in.turboAttackPressed = buttons.turboAttackPressed;
        in.selector = SelectorInput{buttons.selectorUp, buttons.selectorDown, buttons.selectorLeft,
                                    buttons.selectorRight};
        in.menu = readMenuInput(input(), m_config.menu, source);
    }
    const bool wasLeaving = m_tower.leaving();
    const PlayOutcome outcome = m_tower.update(deltaSeconds, inputs);
    // Exit speech begins with the sinking spin, once for the party, and must
    // survive closing PlayScene before the tally. Temple/Underworld and the
    // boss wizard's departure have their own speeches.
    if (!wasLeaving && m_tower.leaving() && !m_towerWorld.isTower() &&
        m_towerWorld.ref().realmId != 5 && m_towerWorld.ref().realmId != 8 &&
        m_tower.victory().stage() <= BossVictory::Stage::Appearing) {
        m_exitSpeech.begin(m_options.unpackedDirectory, m_sounds.get(),
                           ExitRelics::remaining(m_towerWorld.placedItems()), m_tower.party());
    }
    if (outcome == PlayOutcome::Travel) {
        // The party goes on with all it carries; back in the tower it arrives at the way in
        // of the realm it left.
        keepParty();
        Journey journey;
        journey.destination = m_tower.destination();
        journey.party = m_tower.party();
        journey.options.welcome = false;
        journey.options.arriving = true;
        journey.options.arrivalWorld = static_cast<u32>(std::max(m_towerWorld.ref().realmId, 0));
        const bool completedLevel = !m_towerWorld.isTower();
        const auto results = m_tower.levelResults();
        m_tower.close();
        if (completedLevel) {
            const auto* level = m_towerWorld.level();
            const auto maxima =
                level != nullptr ? level->shopMaxima : std::array<s32, 3>{1000, 100, 1000};
            if (!m_afterLevel.open(renderDevice(), context(), journey.party, results, maxima,
                                   m_towerWorld.ref().name)) {
                log::warn("Shop unavailable; keeping level rewards and continuing. Run gdlunpack "
                          "--only SHPDATA.");
            }
        }
        m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
        m_loadingPicture.cover();
        m_journey = std::move(journey);
        return;
    }
    if (outcome == PlayOutcome::GameOver) {
        // Retain checkpoint saves, then end the session rather than resurrecting
        // the defeated party in the tower or awarding a successful-level tally.
        keepParty();
        m_tower.close();
        m_towerWorld.clear();
        m_exitSpeech.close();
        m_attract.gameOver();
        setMaxFrameRate(m_config.display.maxFrameRate);
        startNextAttractScreen();
        return;
    }
    if (outcome == PlayOutcome::Leave) {
        keepParty();
        m_tower.close();
        log::info("Leaving the tower for the title screen");
        if (!startTitleScreen()) {
            startNextAttractScreen();
        }
    }
}

bool Gauntlet::saveSettings(const GameConfig& config) {
    try {
        config.saveFile(GameConfig::userSettingsPath());
        m_config = config;
        m_sounds->setMasterVolume(config.audio.masterVolume);
        m_sounds->setCategoryVolume(SoundCategory::Music, config.audio.musicVolume);
        m_sounds->setCategoryVolume(SoundCategory::Effects, config.audio.effectsVolume);
        m_audio->mixer().setStereo(config.audio.stereo);
        for (auto& controls : m_controls) {
            controls.reset();
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("Could not save settings: {}", e.what());
        return false;
    }
}

void Gauntlet::updatePause(f64 deltaSeconds) {
    const auto source = MenuInputSource::forPlayer(m_pause.player());
    const auto outcome =
        m_pause.update(deltaSeconds, readMenuInput(input(), m_config.menu, source));
    if (outcome == PauseOutcome::Running) {
        return;
    }
    const auto party = m_pause.party();
    // Only successful saves attach the running characters to new slots. A loaded
    // character belongs to the replacement scene, never the one being discarded.
    if (outcome != PauseOutcome::Reload) {
        for (const auto& member : party) {
            m_tower.setSaveSlot(member.player, member.slot);
        }
    }
    m_pause.close();
    m_audio->mixer().setPaused(false);
    for (auto& controls : m_controls) {
        controls.reset();
    }
    if (outcome == PauseOutcome::Resume) {
        return;
    }
    if (outcome == PauseOutcome::Title) {
        keepParty();
        m_tower.close();
        if (!startTitleScreen()) {
            startNextAttractScreen();
        }
        return;
    }
    if (outcome == PauseOutcome::Shop) {
        Journey journey;
        journey.destination = LevelRef::tower();
        journey.party = party;
        journey.options.welcome = false;
        if (const auto* actor = m_tower.actor(source.pad)) {
            journey.options.position = actor->position();
        }
        if (m_afterLevel.open(renderDevice(), context(), party, {}, {1000, 100, 1000},
                              m_towerWorld.ref().name, true)) {
            keepParty();
            m_tower.close();
            m_loadingPicture.load(renderDevice(), m_options.unpackedDirectory);
            m_loadingPicture.cover();
            m_journey = std::move(journey);
        }
        return;
    }
    if (outcome == PauseOutcome::ReturnTower) {
        keepParty();
    }
    // Loading restores characters in the tower, not a snapshot of transient enemies.
    // Do not autosave the discarded level over the character just loaded.
    m_tower.close();
    PlayOptions options;
    options.welcome = false;
    options.arriving = true;
    options.arrivalWorld = static_cast<u32>(std::max(m_towerWorld.ref().realmId, 0));
    if (!startTower(party, options) && !startTitleScreen()) {
        startNextAttractScreen();
    }
}

void Gauntlet::updateAfterLevel(f64 deltaSeconds) {
    ShopSession::Inputs inputs;
    for (s32 player = 0; player < 4; ++player) {
        inputs[static_cast<usize>(player)] =
            readMenuInput(input(), m_config.menu, MenuInputSource::forPlayer(player));
    }
    if (m_afterLevel.update(deltaSeconds, inputs)) {
        GDL_VERIFY(m_journey.has_value(), "Shop requires a pending journey");
        m_journey->party = m_afterLevel.session().party();
        keepParty();
        m_afterLevel.close();
    }
}

void Gauntlet::updateJourney(f64 deltaSeconds) {
    GDL_VERIFY(m_journey.has_value(), "Updating a journey requires a pending journey");
    Journey& journey = *m_journey;
    if (!journey.shown) {
        return;
    }
    if (!journey.presentationStarted) {
        journey.presentationStarted = true;
        m_levelLoading.open(renderDevice(), context(), journey.destination, journey.party);
        journey.movie = m_levelLoading.movie();
        // Present at least one frame of the map before advancing its clock.
        return;
    }
    if (journey.movieStarted) {
        bool skip = false;
        for (const auto& member : journey.party) {
            const auto menu =
                readMenuInput(input(), m_config.menu, MenuInputSource::forPlayer(member.player));
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
    const Journey journey = std::move(*m_journey);
    m_journey.reset();
    m_loadingPicture.release();
    m_levelLoading.close();
    if (!startLevel(journey.destination, journey.party, journey.options) &&
        !startLevel(LevelRef::tower(), journey.party, journey.options) && !startTitleScreen()) {
        startNextAttractScreen();
    }
}

void Gauntlet::onRender(RenderDevice& device) {
    const Extent2D framebuffer = device.framebufferExtent();
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const Mat4 projection =
        makeLetterboxProjection(frameWidth, frameHeight, static_cast<f32>(framebuffer.width),
                                static_cast<f32>(framebuffer.height));
    if (m_idleScreen.isOpen()) {
        m_idleScreen.render(device, projection, frameWidth, frameHeight,
                            glm::radians(m_config.camera.horizontalFovDegrees));
        return;
    }
    if (m_demo.isOpen()) {
        m_demo.render(device, projection, frameWidth, frameHeight);
        return;
    }
    if (m_afterLevel.isOpen()) {
        m_afterLevel.render(device, projection, frameWidth, frameHeight);
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
            m_loadingPicture.draw(m_canvas, width);
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
    if (m_select.isOpen()) {
        m_select.render(device, projection, frameWidth, frameHeight);
        return;
    }
    if (m_tower.isOpen()) {
        m_tower.render(device, projection, frameWidth, frameHeight);
        if (m_pause.isOpen()) {
            m_pause.render(device, projection, frameWidth, frameHeight);
        }
        return;
    }
    m_smokeTest.render(device, projection, static_cast<f32>(clock().totalSeconds()));
}

/** The party's characters go back into the slots they came from (or were first saved to),
 * with all they have gathered; a character never saved has no slot and is not kept. */
void Gauntlet::keepParty() {
    std::vector<PartyMember> party;
    if (m_pause.isOpen()) {
        party = m_pause.party();
    } else if (m_afterLevel.isOpen()) {
        party = m_afterLevel.session().party();
    } else if (m_tower.isOpen()) {
        party = m_tower.party();
    } else if (m_journey.has_value()) {
        party = m_journey->party;
    }
    const bool anySlot = std::ranges::any_of(
        party, [](const PartyMember& member) { return member.slot.has_value(); });
    if (!anySlot || !m_saves.open(m_config.saveDirectory(), m_config.save.slots)) {
        return;
    }
    const usize written = saveParty(m_saves, party);
    log::info("Saved {} of the party to {}", written, m_config.saveDirectory().string());
}

void Gauntlet::onShutdown() {
    m_idleScreen.close();
    m_demo.close();
    keepParty();
    m_pause.close();
    m_movie.close();
    m_title.close();
    m_select.close();
    m_tower.close();
    m_afterLevel.close();
    m_loadingPicture.release();
    m_exitSpeech.close();
    m_levelLoading.close();
    m_journey.reset();
    m_towerWorld.clear();
    m_smokeTest.shutdown();
    m_assets.reset();
    m_sounds.reset();
    m_audio.reset();
}

bool Gauntlet::startMovie(std::string_view name) {
    const auto file = m_assets->find(std::format("{}/{}.avi", kMovieDirectory, name));
    if (!file.has_value()) {
        log::warn("Movie '{}' not found under {}", name, m_assets->root().string());
        return false;
    }
    if (!m_movie.open(renderDevice(), m_audio->mixer(), *file)) {
        return false;
    }
    m_movieActive = true;
    return true;
}

bool Gauntlet::startTitleScreen() {
    m_exitSpeech.close();
    setMaxFrameRate(m_config.display.maxFrameRate);
    if (m_title.open(renderDevice(), context())) {
        m_attract.titleShown();
        return true;
    }
    if (!m_titleWarned) {
        m_titleWarned = true;
        log::warn("Title screen unavailable; unpack the game data into {} with gdlunpack",
                  m_options.unpackedDirectory.string());
    }
    return false;
}

void Gauntlet::startNextAttractScreen() {
    for (usize attempts = 0; attempts < AttractSequencer::kScreenTable.size(); ++attempts) {
        const AttractStep step = m_attract.next();
        if (step.screen == AttractScreen::Flyby || step.screen == AttractScreen::Demo) {
            if (m_demo.openNext(renderDevice(), context())) {
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
