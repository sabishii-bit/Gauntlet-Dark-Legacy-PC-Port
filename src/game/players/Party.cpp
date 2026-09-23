#include "game/players/Party.h"

#include "engine/core/Types.h"

namespace gdl::game {

usize saveParty(SaveSlots& slots, std::span<const PartyMember> party) {
    usize written = 0;
    for (const PartyMember& member : party) {
        if (member.slot.has_value() && *member.slot < slots.count() &&
            slots.write(*member.slot, member.save)) {
            ++written;
        }
    }
    return written;
}

} // namespace gdl::game
