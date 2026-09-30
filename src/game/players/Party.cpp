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

usize recordLevelBeaten(std::span<PartyMember> party, s32 realm, s32 level, s32 rune, s32 legend) {
    usize recorded = 0;
    for (PartyMember& member : party) {
        if (member.fallen) {
            continue;
        }
        member.save.progress().levels.recordBeaten(realm, level, rune, legend);
        ++recorded;
    }
    return recorded;
}

} // namespace gdl::game
