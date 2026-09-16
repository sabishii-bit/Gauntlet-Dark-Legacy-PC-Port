#include "game/Gauntlet.h"

#include <format>
#include <utility>

#include "engine/core/Log.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderTypes.h"

#include "game/MenuInput.h"

namespace gdl::game {

namespace {

constexpr std::string_view kMovieDirectory = "VQMOVIES";
constexpr f64 kFpsReportInterval = 2.0;

} // namespace

Gauntlet::Gauntlet(ApplicationDesc desc, GameOptions options)
    : Application(std::move(desc)), m_options(std::move(options)) {}

void Gauntlet::onInit() {
    m_audio = std::make_unique<AudioDevice>();
    m_sounds = std::make_unique<SoundPlayer>(m_audio->mixer());
    m_assets = std::make_unique<AssetLocator>(assetDirectory());
    m_smokeTest.init(renderDevice());

    if (!m_options.playMovie.empty()) {
        if (!startMovie(m_options.playMovie)) {
            requestQuit();
        }
        return;
    }
    if (m_options.startAtTitle && startTitleScreen()) {
        return;
    }
    startNextAttractScreen();
}

void Gauntlet::onUpdate(f64 deltaSeconds) {
    if (input().wasKeyPressed(Key::Escape)) {
        requestQuit();
    }

    if (m_movieActive) {
        updateMovie(deltaSeconds);
    } else if (m_title.isOpen()) {
        updateTitle(deltaSeconds);
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
    const bool toTitle = m_options.playMovie.empty() && startRequested();
    const bool playing = !toTitle && !skipRequested() && m_movie.update(deltaSeconds);
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
    const TitleOutcome outcome = m_title.update(deltaSeconds, readMenuInput(input()));
    if (outcome == TitleOutcome::Running) {
        return;
    }
    m_title.close();
    if (outcome == TitleOutcome::StartGame) {
        log::info("Player select is not built yet; restarting the attract loop");
        m_attract.reset();
    }
    startNextAttractScreen();
}

void Gauntlet::onRender(RenderDevice& device) {
    const Extent2D framebuffer = device.framebufferExtent();
    const Mat4 projection =
        makeLetterboxProjection(kFrameWidth, kFrameHeight, static_cast<f32>(framebuffer.width),
                                static_cast<f32>(framebuffer.height));
    if (m_movieActive) {
        m_movie.render(device, projection, Rect{0.0f, 0.0f, kFrameWidth, kFrameHeight});
        return;
    }
    if (m_title.isOpen()) {
        m_title.render(device, projection, kFrameWidth, kFrameHeight);
        return;
    }
    m_smokeTest.render(device, projection, static_cast<f32>(clock().totalSeconds()));
}

void Gauntlet::onShutdown() {
    m_movie.close();
    m_title.close();
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
    if (m_title.open(renderDevice(), m_sounds.get(), m_options.unpackedDirectory)) {
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

bool Gauntlet::skipRequested() const {
    if (input().wasKeyPressed(Key::Space)) {
        return true;
    }
    for (int pad = 0; pad < Input::kMaxPads; ++pad) {
        if (input().wasPadButtonPressed(pad, PadButton::A)) {
            return true;
        }
    }
    return false;
}

bool Gauntlet::startRequested() const {
    if (input().wasKeyPressed(Key::Enter)) {
        return true;
    }
    for (int pad = 0; pad < Input::kMaxPads; ++pad) {
        if (input().wasPadButtonPressed(pad, PadButton::Start)) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
