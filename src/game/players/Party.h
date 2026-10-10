#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"
#include "game/players/LevelResults.h"

namespace gdl::game {

/** A locked-in character, the player who drives it and the slot it is saved in, when it is
 * saved at all. */
struct PartyMember {
    s32 player = 0;
    CharacterSave save;
    std::optional<usize> slot = std::nullopt;
    bool fallen = false; ///< died in the levels: it waits in the tower, where it stands again
    f32 turbo = 0.0f;    ///< what its turbo meter starts the level with (none, in the game)
    // Explicit default for aggregate callers that omit transient history.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<s32> helpHeard{}; ///< the help it has had since it was loaded; not saved
    std::optional<LevelResults::Checkpoint> resultsCheckpoint = std::nullopt;
};

/** Writes every member that has a slot back into it; how many were written. */
usize saveParty(SaveSlots& slots, std::span<const PartyMember> party);

/** Records level `level` (from nought) of `realm` beaten, with the runestone and the legend
 * item's realm it holds, by every member still standing (towerRecordLevelBeaten's states 1,
 * 4 and 5: never the fallen, who go on as they came in); how many recorded it. */
usize recordLevelBeaten(std::span<PartyMember> party, s32 realm, s32 level, s32 rune, s32 legend);

} // namespace gdl::game
