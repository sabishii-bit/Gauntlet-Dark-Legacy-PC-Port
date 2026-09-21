#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"

namespace gdl::game {

/** A locked-in character, the player who drives it and the slot it is saved in, when it is
 * saved at all. */
struct PartyMember {
    s32 player = 0;
    CharacterSave save;
    std::optional<usize> slot;
    bool fallen = false; ///< died in the levels: it waits in the tower, where it stands again
    f32 turbo = 0.0f;    ///< what its turbo meter starts the level with (none, in the game)
};

/** Writes every member that has a slot back into it; how many were written. */
usize saveParty(SaveSlots& slots, std::span<const PartyMember> party);

} // namespace gdl::game
