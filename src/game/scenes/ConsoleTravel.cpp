#include "game/scenes/ConsoleTravel.h"

namespace gdl::game {

namespace {

/** Whether `level` is the campaign's. A Legends map walked has no realm in it and cannot wait
 * behind a challenge: the challenge's opening ends the visit. */
bool inCampaign(const LevelRef& level) {
    return level.realmId >= 0;
}

} // namespace

ConsoleTravelKind consoleTravelKind(const LevelRef& from, const LevelRef& to,
                                    const std::optional<LevelRef>& parent) {
    if (from.isSecret() && parent.has_value() && to == *parent) {
        return ConsoleTravelKind::ReturnToParent;
    }
    if (to.isSecret()) {
        if (from.isSecret()) {
            return ConsoleTravelKind::ChallengeToChallenge;
        }
        return inCampaign(from) ? ConsoleTravelKind::EnterChallenge : ConsoleTravelKind::Plain;
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
