#include "game/Gauntlet.h"

#include <format>
#include <utility>

#include "engine/core/Log.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

namespace {

constexpr std::string_view kMovieDirectory = "VQMOVIES";
constexpr f64 kFpsReportInterval = 2.0;

} // namespace

Gauntlet::Gauntlet(ApplicationDesc desc, GameOptions options)
    : Application(std::move(desc)), m_options(std::move(options)) {}

void Gauntlet::onInit() {
    m_audio = std::make_unique<AudioDevice>();
    m_assets = std::make_unique<AssetLocator>(assetDirectory());
    m_smokeTest.init(renderDevice());

    if (!m_options.playMovie.empty()) {
        if (!startMovie(m_options.playMovie)) {
            requestQuit();
        }
        return;
    }
    startNextAttractScreen();
}

void Gauntlet::onUpdate(f64 deltaSeconds) {
    if (input().wasKeyPressed(Key::Escape)) {
        requestQuit();
    }

    if (m_movieActive) {
        const bool playing = !skipRequested() && m_movie.update(deltaSeconds);
        if (!playing) {
            m_movie.close();
            m_movieActive = false;
            if (!m_options.playMovie.empty()) {
                requestQuit();
            } else {
                startNextAttractScreen();
            }
        }
    }

    m_fpsAccumulator += deltaSeconds;
    ++m_fpsFrames;
    if (m_fpsAccumulator >= kFpsReportInterval) {
        log::trace("{:.1f} fps", static_cast<f64>(m_fpsFrames) / m_fpsAccumulator);
        m_fpsAccumulator = 0.0;
        m_fpsFrames = 0;
    }
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
    m_smokeTest.render(device, projection, static_cast<f32>(clock().totalSeconds()));
}

void Gauntlet::onShutdown() {
    m_movie.close();
    m_smokeTest.shutdown();
    m_assets.reset();
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

void Gauntlet::startNextAttractScreen() {
    for (usize attempts = 0; attempts < AttractSequencer::kScreenTable.size(); ++attempts) {
        const AttractStep step = m_attract.next();
        if (step.movie.empty()) {
            continue;
        }
        if (startMovie(step.movie)) {
            return;
        }
    }
    log::warn("No attract movie could be played; showing the smoke test scene");
}

bool Gauntlet::skipRequested() const {
    if (input().wasKeyPressed(Key::Enter) || input().wasKeyPressed(Key::Space)) {
        return true;
    }
    for (int pad = 0; pad < Input::kMaxPads; ++pad) {
        if (input().wasPadButtonPressed(pad, PadButton::Start) ||
            input().wasPadButtonPressed(pad, PadButton::A)) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
