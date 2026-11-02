#pragma once

#include "game/characters/CharacterProfile.h"
#include "game/rollback/InputCommand.h"

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
    static constexpr u8 kVersion = 2;
    static constexpr usize kHeaderBytes = 16;
    static constexpr usize kMinBytes = kHeaderBytes + CharacterProfilePacket::kMinBytes;
    static constexpr usize kMaxBytes = kHeaderBytes + CharacterProfilePacket::kMaxBytes;
    static bool recognizes(std::span<const u8> bytes);
    static std::optional<std::vector<u8>> encode(const PartyCheckpoint& checkpoint);
    static std::optional<PartyCheckpoint> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
