#pragma once

#include <string_view>

#include "engine/core/Types.h"

#include "game/screens/ShopSession.h"

namespace gdl::game {
/** game_main's completed-level route: init_gamemovie (when a final boss fell),
 * init_shop(0), then init_player_select(2). The movie is not an attract screen. */
class LevelCompletion {
public:
    enum class Stage : u8 { Movie, Results, Characters, Done };

    LevelCompletion(s32 bossKind, bool defeated)
        : m_movie(defeated ? movieOf(bossKind) : std::string_view{}),
          m_stage(!m_movie.empty() ? Stage::Movie : Stage::Results),
          m_visit(defeated && bossKind == 44 ? ShopVisit::Completion : ShopVisit::Level) {}

    Stage stage() const { return m_stage; }
    std::string_view movie() const { return m_movie; }
    ShopVisit visit() const { return m_visit; }
    /** Movie end (or unavailable media), all results accepted, all character menus done. */
    void advance() {
        switch (m_stage) {
        case Stage::Movie: m_stage = Stage::Results; break;
        case Stage::Results: m_stage = Stage::Characters; break;
        case Stage::Characters: m_stage = Stage::Done; break;
        case Stage::Done: break;
        }
    }

private:
    // auxscreen.c init_gamemovie: E_GARM -> victory, E_SKORNE2 -> garm.
    static std::string_view movieOf(s32 kind) {
        switch (kind) {
        case 44: return "victory";
        case 43: return "garm";
        default: return "";
        }
    }
    std::string_view m_movie;
    Stage m_stage;
    ShopVisit m_visit;
};
} // namespace gdl::game
