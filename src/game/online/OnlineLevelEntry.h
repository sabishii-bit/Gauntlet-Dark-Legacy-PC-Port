#pragma once

#include "game/scenes/LevelLoadingScreen.h"
#include "game/scenes/MovieScene.h"

namespace gdl::game {
/** Local native presentation inside the match's loading barrier. No simulation,
 * network asset paths or progression writes. Ready means this viewer finished;
 * gameplay still waits for every peer's Ready/Commit handshake. */
class OnlineLevelEntry {
public:
    enum class Phase : u8 { Closed, Map, Movie, Ready, Failed };
    /** Ticks the entry waits for its first frame to show before it goes on regardless: a
     * machine that cannot present (a minimised window) must not hold the others. */
    static constexpr u32 kUnshownTicks = 60;
    OnlineLevelEntry() = default;
    OnlineLevelEntry(const OnlineLevelEntry&) = delete;
    OnlineLevelEntry& operator=(const OnlineLevelEntry&) = delete;
    OnlineLevelEntry(OnlineLevelEntry&&) = delete;
    OnlineLevelEntry& operator=(OnlineLevelEntry&&) = delete;
    ~OnlineLevelEntry() { close(); }
    void open(RenderDevice& device, const GameServices& services, const LevelRef& level,
              std::span<const PartyMember> party, bool transitionOnly = false);
    void close();
    /** skipMovie is the host-confirmed unanimous result, never a local button press. */
    void update(RenderDevice& device, const GameServices& services, bool skipMovie);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    Phase phase() const { return m_phase; }
    bool visible() const { return m_loading.active() || m_movie.isOpen(); }

private:
    LevelLoadingScreen m_loading;
    MovieScene m_movie;
    Phase m_phase = Phase::Closed;
    bool m_shown = false;
    u32 m_unshown = 0; ///< ticks waited on the first frame
    bool m_wantsMovie = false;
};
} // namespace gdl::game
