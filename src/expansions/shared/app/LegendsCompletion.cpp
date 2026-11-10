#include "expansions/shared/app/LegendsCompletion.h"

#include <algorithm>
#include <utility>

#include "engine/platform/Paths.h"

#include "expansions/dreamcast/level/LegendsDreamcastPortals.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/scenes/AfterLevelScene.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayerSelectScene.h"

namespace gdl::game {
std::optional<LegendsCompletedTravel> completedLegendsTravel(const PlayScene& scene) {
    const auto* level =
        scene.world() ? dynamic_cast<const LegendsLevel*>(scene.world()->imported()) : nullptr;
    if (!scene.isOpen() || !scene.leaving() || !level || level->adapter().isTower() ||
        !level->destination()) {
        return std::nullopt;
    }
    if (level->platform() == LegendsPlatform::Dreamcast &&
        dreamcastContinuesRun(paths::utf8(level->directory().filename()), *level->destination())) {
        return std::nullopt;
    }
    const auto departure = level->departure();
    if (departure ? !departure->covered(scene.transition().covering())
                  : !scene.transition().covering() || !scene.departure().finished()) {
        return std::nullopt;
    }
    auto party = scene.party();
    if (std::ranges::none_of(party, [](const PartyMember& member) { return !member.fallen; })) {
        return std::nullopt;
    }
    auto results = scene.levelResults();
    level->completeTravel(party);
    // The captured tally owns the whole accumulated run, including prior
    // segments. The next map must start a new baseline after shopping.
    for (auto& member : party) {
        member.resultsCheckpoint.reset();
    }
    return LegendsCompletedTravel{level->platform(), level->directory(),
                                  level->directory().parent_path() / *level->destination(),
                                  std::move(party), std::move(results)};
}

std::vector<PartyMember> legendsCompletionParty(std::span<const PartyMember> completed,
                                                const AfterLevelScene& shop,
                                                const PlayerSelectScene& characters) {
    if (characters.isOpen()) {
        return characters.party();
    }
    if (shop.isOpen()) {
        return shop.session().party();
    }
    return {completed.begin(), completed.end()};
}
} // namespace gdl::game
