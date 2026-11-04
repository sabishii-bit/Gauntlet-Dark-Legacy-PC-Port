#pragma once

#include <string_view>

#include "engine/core/Types.h"

#include "game/level/BossKinds.h"
#include "game/shop/ShopSession.h"

namespace gdl::game {
/** game_main's completed-level route: init_gamemovie (when a final boss fell),
 * init_shop(0), then init_player_select(2). The movie is not an attract screen. */
class LevelCompletion {
public:
    enum class Stage : u8 { Movie, Results, Characters, Done };

    LevelCompletion(s32 bossKind, bool defeated)
        : m_movie(defeated ? movieOf(bossKind) : std::string_view{}),
          m_stage(!m_movie.empty() ? Stage::Movie : Stage::Results),
          m_visit(defeated && completesGame(bossKind) ? ShopVisit::Completion : ShopVisit::Level) {}

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
    /** The movie a final boss's fall plays, empty for every other code. */
    static std::string_view movieOf(s32 kind) {
        const BossStage* stage = bossStageOf(kind);
        return stage != nullptr && stage->finale.has_value() ? stage->finale->movie
                                                             : std::string_view{};
    }
    /** Whether the fall of the boss `kind` names completes the game. */
    static bool completesGame(s32 kind) {
        const BossStage* stage = bossStageOf(kind);
        return stage != nullptr && stage->finale.has_value() && stage->finale->completesGame;
    }
    std::string_view m_movie;
    Stage m_stage;
    ShopVisit m_visit;
};
} // namespace gdl::game
