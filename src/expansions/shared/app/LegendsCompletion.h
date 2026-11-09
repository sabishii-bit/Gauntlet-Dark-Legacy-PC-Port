#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/characters/Party.h"

namespace gdl::game {
class PlayScene;
class AfterLevelScene;
class PlayerSelectScene;

/** A completed native departure waiting for the common tally and shop. The
 * source scene remains parked until its destination has opened successfully. */
struct LegendsCompletedTravel {
    LegendsPlatform platform = LegendsPlatform::N64;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::vector<PartyMember> party;
    std::vector<LevelResults> results;
};

/** Captures the real scene's cumulative results and records completion only on
 * the departing party. Tower admission, quitting, and unfinished ceremonies
 * never produce a results visit. Direct Skorne approaches retain their tally
 * checkpoint for the boss instead. Does not mutate the parked scene or disk. */
std::optional<LegendsCompletedTravel> completedLegendsTravel(const PlayScene& scene);

/** During completion the parked gameplay actors are no longer authoritative.
 * Preserve the current menu's purchases and selected save slots on shutdown;
 * a selector whose players all quit intentionally returns an empty party. */
std::vector<PartyMember> legendsCompletionParty(std::span<const PartyMember> completed,
                                                const AfterLevelScene& shop,
                                                const PlayerSelectScene& characters);
} // namespace gdl::game
