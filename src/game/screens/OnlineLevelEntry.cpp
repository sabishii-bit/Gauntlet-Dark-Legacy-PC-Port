#include "game/screens/OnlineLevelEntry.h"

#include <format>

#include "engine/core/Log.h"

namespace gdl::game {
void OnlineLevelEntry::open(RenderDevice& device, const GameContext& context, const LevelRef& level,
                            std::span<const PartyMember> party) {
    close();
    m_loading.open(device, context, level, party);
    m_wantsMovie = LevelLoadingScreen::movieWanted(m_loading.movie(), party);
    m_phase = m_loading.active() ? Phase::Map : Phase::Ready;
    log::info("Online entry: {} {}", level.name, m_loading.active() ? "map" : "ready");
}
void OnlineLevelEntry::close() {
    m_movie.close();
    m_loading.close();
    m_phase = Phase::Closed;
    m_shown = false;
    m_wantsMovie = false;
}
void OnlineLevelEntry::update(RenderDevice& device, const GameContext& context, bool skipMovie) {
    if (!m_shown) {
        return; // Present the map/first movie frame before advancing its clock.
    }
    if (m_phase == Phase::Movie) {
        if (skipMovie || !m_movie.update(1.0 / 60)) {
            // Retain the last video frame while another peer is still viewing/loading.
            m_movie.setVolume(0);
            m_phase = Phase::Ready;
            log::info("Online entry: movie finished{}", skipMovie ? " (unanimous skip)" : "");
        }
    } else if (m_phase == Phase::Map && m_loading.update(1.0f / 60)) {
        if (!m_wantsMovie) {
            m_phase = Phase::Ready;
            return;
        }
        const AssetLocator fallback(context.unpackedRoot);
        const auto& assets = context.assets != nullptr ? *context.assets : fallback;
        const auto file = assets.find(std::format("VQMOVIES/{}.avi", m_loading.movie()));
        if (!file || !m_movie.open(device, context.movieMixer, *file)) {
            log::error("Online entry movie unavailable: {}", m_loading.movie());
            m_phase = Phase::Failed;
            return;
        }
        m_movie.setVolume(context.config != nullptr ? context.config->audio.masterVolume *
                                                          context.config->audio.movieVolume
                                                    : 1);
        log::info("Online entry: movie {}", m_loading.movie());
        m_loading.close();
        m_phase = Phase::Movie;
        m_shown = false;
    }
}
void OnlineLevelEntry::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (m_movie.isOpen()) {
        // Movie texture uploads must precede every other draw in the Vulkan frame.
        m_movie.render(device, projection, {0, 0, width, height});
    } else if (m_loading.active()) {
        Canvas canvas;
        canvas.begin(device, makeVirtualScreenTransform(projection, 512, 384, width, height));
        m_loading.draw(canvas, device);
        canvas.end();
    }
    m_shown = true;
}
} // namespace gdl::game
