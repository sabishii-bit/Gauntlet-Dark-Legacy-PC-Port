#include "game/app/Gauntlet.h"

#include "game/app/Scenario.h"

#include <filesystem>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/PngImage.h"
#include "engine/core/Log.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderTypes.h"

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
    m_sounds = std::make_unique<SoundPlayer>(m_audio->mixer());
    m_sounds->setMasterVolume(m_config.audio.masterVolume);
    m_sounds->setCategoryVolume(SoundCategory::Music, m_config.audio.musicVolume);
    m_sounds->setCategoryVolume(SoundCategory::Effects, m_config.audio.effectsVolume);
    m_assets = std::make_unique<AssetLocator>(assetDirectory());
    m_smokeTest.init(renderDevice());

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
    context.unpackedRoot = m_options.unpackedDirectory;
    return context;
}

void Gauntlet::onUpdate(f64 deltaSeconds) {
    // Escape quits, except while a name is being typed, where it leaves the name instead.
    if (readMenuInput(input(), m_config.menu).escape && !(m_select.isOpen() && m_select.typing())) {
        requestQuit();
    }

    if (m_movieActive) {
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

void Gauntlet::updateMovie(f64 deltaSeconds) {
    const MenuInput menu = readMenuInput(input(), m_config.menu);
    const bool toTitle = m_options.playMovie.empty() && menu.start;
    const bool playing = !toTitle && !menu.select && m_movie.update(deltaSeconds);
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
            party.push_back(PartyMember{player, lane.save()});
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

bool Gauntlet::startTower(std::span<const PartyMember> party, const TowerOptions& options) {
    if (party.empty()) {
        return false;
    }
    if (m_tower.open(renderDevice(), context(), m_towerWorld, party, options)) {
        log::info("Every player is ready; entering the tower");
        setMaxFrameRate(m_config.timing.gameplayFrameRate);
        return true;
    }
    log::warn("The tower is unavailable; unpack the levels with gdlunpack --levels");
    return false;
}

void Gauntlet::updateTower(f64 deltaSeconds) {
    TowerScene::Inputs inputs;
    for (s32 player = 0; player < TowerScene::kPlayerCount; ++player) {
        const MenuInputSource source = MenuInputSource::forPlayer(player);
        PlayInput& in = inputs[static_cast<usize>(player)];
        in.move = readMoveInput(input(), m_config.play, source.keyboard, source.pad);
        in.menu = readMenuInput(input(), m_config.menu, source);
    }
    if (m_tower.update(deltaSeconds, inputs) == TowerOutcome::Leave) {
        m_tower.close();
        log::info("Leaving the tower for the title screen");
        if (!startTitleScreen()) {
            startNextAttractScreen();
        }
    }
}

void Gauntlet::onRender(RenderDevice& device) {
    const Extent2D framebuffer = device.framebufferExtent();
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const Mat4 projection =
        makeLetterboxProjection(frameWidth, frameHeight, static_cast<f32>(framebuffer.width),
                                static_cast<f32>(framebuffer.height));
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
        return;
    }
    m_smokeTest.render(device, projection, static_cast<f32>(clock().totalSeconds()));
}

void Gauntlet::onShutdown() {
    m_movie.close();
    m_title.close();
    m_select.close();
    m_tower.close();
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
    setMaxFrameRate(m_config.display.maxFrameRate);
    if (m_title.open(renderDevice(), context())) {
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
