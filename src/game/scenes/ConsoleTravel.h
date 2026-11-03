#pragma once
#include <optional>

#include "engine/core/Types.h"

#include "game/level/LevelCatalog.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** What console travel does with the level a secret challenge was entered from, its parent. */
enum class ConsoleTravelKind : u8 {
    Plain,                ///< goes there; a level that is no challenge lets the parent go
    EnterChallenge,       ///< into a challenge from a level or the tower, which waits as its
                          ///< parent; from a Legends map walked the travel is plain
    ChallengeToChallenge, ///< from one challenge into another, the parent still waiting
    ReturnToParent,       ///< from a challenge back to its waiting parent, which takes up again
};

/** How console travel from `from` to `to` treats `parent`, the level waiting behind a challenge:
 * the rule both play modes share. */
ConsoleTravelKind consoleTravelKind(const LevelRef& from, const LevelRef& to,
                                    const std::optional<LevelRef>& parent);

/** How console travel takes the party where a travel request names. */
struct ConsoleJourney {
    ConsoleTravelKind kind = ConsoleTravelKind::Plain;
    bool secret = false; ///< into a challenge: the loading screen's secret presentation, and
                         ///< the parent taken up again when the challenge cannot load
    PlayOptions options; ///< no welcome and no portal arrival, standing at the request's
                         ///< place facing its way when it names them
};

/** The journey `request` makes from `from`, with `parent` waiting behind a challenge. */
ConsoleJourney consoleJourney(const PlayScene::ConsoleTravelRequest& request, const LevelRef& from,
                              const std::optional<LevelRef>& parent);

} // namespace gdl::game
