#pragma once

#include <filesystem>
#include <string_view>

#include "game/level/LevelCatalog.h"
#include "game/scenes/MovieScene.h"
#include "game/shop/ShopSession.h"

namespace gdl::game {
struct GameContext;

/** Local ending presentation inside the shared results-loading barrier. This owns
 * neither rewards, saves nor the party: Ready permits the caller to load results,
 * which still wait for every peer's acknowledgement before accepting input. */
class OnlineCompletion {
public:
    enum class Phase : u8 { Closed, Movie, Ready, Failed };
    /** Ticks the movie waits for its first frame to show before it plays regardless: a
     * machine that cannot present (a minimised window) must not hold the others. */
    static constexpr u32 kUnshownTicks = 60;
    struct Route {
        std::string_view movie;
        ShopVisit visit = ShopVisit::Level;
        u32 arrivalWorld = 0;
        bool skippable = false;
    };

    OnlineCompletion() = default;
    OnlineCompletion(const OnlineCompletion&) = delete;
    OnlineCompletion& operator=(const OnlineCompletion&) = delete;
    OnlineCompletion(OnlineCompletion&&) = delete;
    OnlineCompletion& operator=(OnlineCompletion&&) = delete;
    ~OnlineCompletion() { close(); }

    /** Source and boss state are locally validated campaign data, not network paths. */
    static Route route(const LevelRef& source, s32 bossKind, bool defeated);
    void open(RenderDevice& device, const GameContext& context, const LevelRef& source,
              s32 bossKind, bool defeated);
    void close();
    /** A confirmed unanimous vote cannot skip an unskippable retail ending. */
    void update(const GameContext& context, bool unanimousSkip = false);
    /** First draw of the frame; the final frame remains visible while peers finish. */
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);

    Phase phase() const { return m_phase; }
    bool visible() const { return m_movie.isOpen(); }
    const Route& selection() const { return m_route; }
    const std::filesystem::path& moviePath() const { return m_path; }

private:
    MovieScene m_movie;
    Route m_route;
    std::filesystem::path m_path;
    Phase m_phase = Phase::Closed;
    bool m_shown = false;
    u32 m_unshown = 0; ///< ticks waited on the first frame
};
} // namespace gdl::game
