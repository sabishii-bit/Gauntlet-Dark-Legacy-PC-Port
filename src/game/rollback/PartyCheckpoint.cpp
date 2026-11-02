#include "game/rollback/PartyCheckpoint.h"

#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDPC");
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
    bytes.reserve(kHeaderBytes + profile->size());
    ByteWriter out(bytes);
    out.writeU32(kMagic);
    out.writeU8(kVersion);
    out.writeU8(checkpoint.seat);
    out.writeU16(0);
    out.writeU64(checkpoint.epoch);
    out.writeBytes(*profile);
    return bytes;
}
std::optional<PartyCheckpoint> PartyCheckpointPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kMinBytes || bytes.size() > kMaxBytes || !recognizes(bytes)) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    reader.readU32();
    if (reader.readU8() != kVersion) {
        return std::nullopt;
    }
    const u8 seat = reader.readU8();
    if (seat >= InputCommand::kSeats || reader.readU16() != 0) {
        return std::nullopt;
    }
    const u64 epoch = reader.readU64();
    auto profile = CharacterProfilePacket::decode(reader.readBytes(reader.remaining()));
    if (epoch == 0 || !profile) {
        return std::nullopt;
    }
    return PartyCheckpoint{epoch, seat, std::move(*profile)};
}
} // namespace gdl::game
