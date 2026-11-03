#include "game/scenes/ConsoleTravel.h"

namespace gdl::game {

ConsoleTravelKind consoleTravelKind(const LevelRef& from, const LevelRef& to,
                                    const std::optional<LevelRef>& parent) {
    if (from.isSecret() && parent.has_value() && to == *parent) {
        return ConsoleTravelKind::ReturnToParent;
    }
    if (to.isSecret()) {
        return from.isSecret() ? ConsoleTravelKind::ChallengeToChallenge
                               : ConsoleTravelKind::EnterChallenge;
    }
    return ConsoleTravelKind::Plain;
}

ConsoleJourney consoleJourney(const PlayScene::ConsoleTravelRequest& request, const LevelRef& from,
                              const std::optional<LevelRef>& parent) {
    ConsoleJourney journey;
    journey.kind = consoleTravelKind(from, request.destination, parent);
    journey.secret = journey.kind == ConsoleTravelKind::EnterChallenge ||
                     journey.kind == ConsoleTravelKind::ChallengeToChallenge;
    journey.options.welcome = false;
    journey.options.position = request.position;
    journey.options.yaw = request.yaw;
    return journey;
}

} // namespace gdl::game
