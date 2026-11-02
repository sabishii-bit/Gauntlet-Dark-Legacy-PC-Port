#include "game/netplay/OnlineCompletion.h"

#include <algorithm>
#include <format>

#include "engine/core/Log.h"
#include "engine/platform/Paths.h"

#include "game/scenes/GameContext.h"

namespace gdl::game {
namespace {
f32 movieVolume(const GameContext& context) {
    return context.config != nullptr
               ? context.config->audio.masterVolume * context.config->audio.movieVolume
               : 1;
}
} // namespace

OnlineCompletion::Route OnlineCompletion::route(const LevelRef& source, s32 bossKind,
                                                bool defeated) {
    Route result;
    if (defeated) {
        switch (bossKind) {
        case 43: result.movie = "garm"; break;
        case 44:
            result.movie = "victory";
            result.visit = ShopVisit::Completion;
            break;
        default: break;
        }
    }
    // init_next_level remembers the last non-Tower realm (800563DC-800563F8).
    // EnterTower maps it through crystal_order (800A33D0-800A340C); the normal
    // arrivalPoint/TowerAccess path also preserves SetPlayerStartPos's locked fallback.
    // Thus Garm returns beside Battlefield (realm 8), not the movie's destination.
    result.arrivalWorld = static_cast<u32>(std::max(source.realmId, 0));
    return result;
}

void OnlineCompletion::open(RenderDevice& device, const GameContext& context,
                            const LevelRef& source, s32 bossKind, bool defeated) {
    close();
    m_route = route(source, bossKind, defeated);
    if (m_route.movie.empty()) {
        m_phase = Phase::Ready;
        return;
    }
    const AssetLocator fallback(context.unpackedRoot);
    const auto& assets = context.assets != nullptr ? *context.assets : fallback;
    const auto file = assets.find(std::format("VQMOVIES/{}.avi", m_route.movie));
    if (!file) {
        log::error("Online completion movie unavailable: {}", m_route.movie);
        m_phase = Phase::Failed;
        return;
    }
    m_path = *file;
    if (!m_movie.open(device, context.movieMixer, m_path)) {
        log::error("Online completion movie could not open: {}", paths::utf8(m_path));
        m_phase = Phase::Failed;
        return;
    }
    m_movie.setVolume(movieVolume(context));
    m_phase = Phase::Movie;
    log::info("Online completion: movie {}", m_route.movie);
}

void OnlineCompletion::close() {
    m_movie.close();
    m_route = {};
    m_path.clear();
    m_phase = Phase::Closed;
    m_shown = false;
    m_unshown = 0;
}

void OnlineCompletion::update(const GameContext& context, bool unanimousSkip) {
    if (m_phase != Phase::Movie || (!m_shown && ++m_unshown < kUnshownTicks)) {
        return;
    }
    m_movie.setVolume(movieVolume(context));
    // init_gamemovie sets movie_state=2 for both endings (80019D48-80019D80).
    // do_gamemovie permits a skip only for state 1 (80019C68-80019C78).
    if ((unanimousSkip && m_route.skippable) || !m_movie.update(1.0 / 60)) {
        m_movie.setVolume(0);
        m_phase = Phase::Ready;
        log::info("Online completion: movie finished {}", m_route.movie);
    }
}

void OnlineCompletion::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (m_movie.isOpen()) {
        m_movie.render(device, projection, {0, 0, width, height});
        m_shown = true;
    }
}
} // namespace gdl::game
