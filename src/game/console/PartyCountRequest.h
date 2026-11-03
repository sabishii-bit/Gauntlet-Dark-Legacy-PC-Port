#pragma once
#include <string>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"

namespace gdl::game {

/** What `party_count` asks: the party size the level sets itself up for. */
struct PartyCountRequest {
    enum class Mode : u8 {
        Players, ///< follow the players present again
        Fixed    ///< set the level up for `players`
    };
    Mode mode = Mode::Players;
    s32 players = 0; ///< the party size, when it is fixed
    bool operator==(const PartyCountRequest&) const = default;
};

std::string partyCountUsage();
/** Reads `party_count`'s one argument, a count from one to four or off; its usage for
 * anything else. */
gdl::Expected<PartyCountRequest, std::string> parsePartyCount(ConsoleCommand::Arguments arguments);

} // namespace gdl::game
