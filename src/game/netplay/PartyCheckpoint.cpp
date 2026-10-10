#include "game/netplay/PartyCheckpoint.h"

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDPC");
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
} // namespace
bool PartyCheckpointPacket::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}
std::optional<std::vector<u8>> PartyCheckpointPacket::encode(const PartyCheckpoint& checkpoint) {
    const auto profile = CharacterProfilePacket::encode(checkpoint.profile);
    if (checkpoint.epoch == 0 || checkpoint.seat >= InputCommand::kSeats || !profile) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    word(bytes, kMagic);
    bytes.insert(bytes.end(), {1, checkpoint.seat, 0, 0});
    word(bytes, static_cast<u32>(checkpoint.epoch));
    word(bytes, static_cast<u32>(checkpoint.epoch >> 32U));
    bytes.insert(bytes.end(), profile->begin(), profile->end());
    return bytes;
}
std::optional<PartyCheckpoint> PartyCheckpointPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes || !recognizes(bytes)) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    reader.readU32();
    if (reader.readU8() != 1) {
        return std::nullopt;
    }
    const u8 seat = reader.readU8();
    if (seat >= InputCommand::kSeats || reader.readU16() != 0) {
        return std::nullopt;
    }
    const u64 low = reader.readU32();
    const u64 epoch = low | (u64{reader.readU32()} << 32U);
    auto profile = CharacterProfilePacket::decode(reader.readBytes(CharacterProfilePacket::kBytes));
    if (epoch == 0 || !profile) {
        return std::nullopt;
    }
    return PartyCheckpoint{epoch, seat, std::move(*profile)};
}
} // namespace gdl::game
