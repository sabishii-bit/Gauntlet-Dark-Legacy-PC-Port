#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "game/players/CharacterSave.h"

namespace gdl::game {

/** A locked-in character, the player who drives it and the slot it is saved in, when it is
 * saved at all. */
struct PartyMember {
    int player = 0;
    CharacterSave save;
    std::optional<std::size_t> slot = std::nullopt;
    bool fallen = false; ///< died in the levels: it waits in the tower, where it stands again
    float turbo = 0.0f;  ///< what its turbo meter starts the level with (none, in the game)
    // Explicit default for aggregate callers that omit transient history.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<int> helpHeard{}; ///< the help it has had since it was loaded; not saved
};

/** Writes every member that has a slot back into it; how many were written. */
std::size_t saveParty(SaveSlots& slots, std::span<const PartyMember> party);

} // namespace gdl::game
