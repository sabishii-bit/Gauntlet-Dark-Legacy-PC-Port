#pragma once
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/party/PlayerPowerups.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::test {
/** One update of the party's powerups as the scene runs it: what is worn as the update begins,
 * then the clock once the party and its opponents have had their turns; the endings heard. */
inline std::vector<game::PowerupEnding> powerupUpdate(std::span<game::PlayerRuntime> players,
                                                      f32 seconds,
                                                      game::PlayerPowerups::Clock clock) {
    auto endings = game::PlayerPowerups::wear(players, seconds, clock);
    game::PlayerPowerups::runClock(players, seconds, clock);
    return endings;
}
} // namespace gdl::test
