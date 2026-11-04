#pragma once

#include <array>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "fixtures/PacketStar.h"
#include "game/characters/CharacterProfile.h"
#include "game/netplay/MatchSession.h"
#include "game/rollback/PartyCheckpoint.h"
#include "game/rollback/RollbackSessionHarness.h"

namespace gdl::test {
/** Match sessions over a packet star, each bound to an input harness. */
struct MatchSessionRoom : PacketStar<> {
    std::array<game::MatchSession, kSlots> sessions;
    std::array<RollbackSessionHarness, kSlots> inputs;
    std::array<u64, kSlots> bound{}; ///< the epoch each peer's harness was last bound for

    using PacketStar::PacketStar;

    /** The party every peer offers, under `name`. */
    game::MatchParty party(std::string_view name) const {
        game::MatchParty result;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                game::CharacterProfile profile;
                profile.name = std::string{name};
                profile.character = static_cast<s32>(seat);
                profile.gold = static_cast<s32>(seat) * 100;
                result[seat] = profile;
            }
        }
        return result;
    }
};
} // namespace gdl::test
