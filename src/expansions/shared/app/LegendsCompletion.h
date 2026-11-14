#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/characters/Party.h"

namespace gdl::game {
class PlayScene;
class AfterLevelScene;
class PlayerSelectScene;
class ImportedLevel;
struct CharacterSave;

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

/** Keeps, in each member of a party giving up a Legends level's earnings (a game over, the
 * title, a shutdown), the permanent discoveries its live save (`live`, none for a member
 * without one) made in the campaign of `level`, the N64's or the Dreamcast's; false, the
 * party left as it is, when `level` is no Legends level. */
bool keepLegendsVisitProgress(const ImportedLevel* level, std::span<PartyMember> party,
                              const std::function<const CharacterSave*(s32 player)>& live);
} // namespace gdl::game
