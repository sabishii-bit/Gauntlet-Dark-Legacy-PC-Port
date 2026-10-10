#pragma once

#include "game/netplay/CharacterProfile.h"
#include "game/netplay/InputCommand.h"

namespace gdl::game {
using MatchParty = std::array<std::optional<CharacterProfile>, InputCommand::kSeats>;

/** One seat of an authoritative travel checkpoint. Reliable ordering places all
 * occupied seats after Prepare and before Ready; no disk or device identifiers. */
struct PartyCheckpoint {
    u64 epoch = 0;
    u8 seat = 0;
    CharacterProfile profile;
};
class PartyCheckpointPacket {
public:
    static constexpr usize kBytes = 16 + CharacterProfilePacket::kBytes;
    static bool recognizes(std::span<const u8> bytes);
    static std::optional<std::vector<u8>> encode(const PartyCheckpoint& checkpoint);
    static std::optional<PartyCheckpoint> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
