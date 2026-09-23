#include "game/players/Party.h"

#include <cstddef>

namespace gdl::game {

std::size_t saveParty(SaveSlots& slots, std::span<const PartyMember> party) {
    std::size_t written = 0;
    for (const PartyMember& member : party) {
        if (member.slot.has_value() && *member.slot < slots.count() &&
            slots.write(*member.slot, member.save)) {
            ++written;
        }
    }
    return written;
}

} // namespace gdl::game
