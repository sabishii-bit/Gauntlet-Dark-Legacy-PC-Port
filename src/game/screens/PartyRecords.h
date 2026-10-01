#pragma once

#include <span>
#include <vector>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"

#include "game/players/LevelResults.h"
#include "game/players/Party.h"
#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

/**
 * What a level makes of the party's characters and hands on: experience won (scaled by the
 * place, feeding the turbo meter when won by a kill), the party as it stands for the next
 * level (the fallen as they came in, keeping what they were taught), the party as it gives a
 * level up, and what each standing member gained there for the tally.
 */
class PartyRecords {
public:
    /** Gives `player`'s character experience won in play, as the original awards it: scaled by
     * `level` (its own scale, less the further the character is past the level the place is
     * meant for); what a kill wins also feeds the turbo meter, unless the character is in the
     * middle of a turbo move. The fallen win nothing. */
    static void award(std::span<PlayerRuntime> players, s32 player, s32 amount, bool kill,
                      const LevelInfo* level);
    static void destroyedGenerator(std::span<PlayerRuntime> players, s32 player);
    /** Called only while players are in live gameplay, not paused or in a menu. */
    static void advanceTime(std::span<PlayerRuntime> players, f64 seconds);
    /** The party as it stands, with all it has gathered, for the next level. */
    static std::vector<PartyMember> members(std::span<const PlayerRuntime> players);
    /** `party` as it leaves a level it gives up: everyone as they came in, keeping only what
     * they were taught and the slots they are kept in (kill_player, then PlayerRestoreState
     * in the tower). */
    static std::vector<PartyMember> abandoned(std::span<const PlayerRuntime> players,
                                              std::span<const PartyMember> party);
    /** What each standing member gained in the level. */
    static std::vector<LevelResults> results(std::span<const PlayerRuntime> players);
};

} // namespace gdl::game
