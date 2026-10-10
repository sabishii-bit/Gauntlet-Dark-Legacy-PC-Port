#include "game/netplay/MotionSnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLM");
static_assert(sizeof(f32) == 4 && std::numeric_limits<f32>::is_iec559);

void writeU32(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void writeU64(std::vector<u8>& bytes, u64 value) {
    writeU32(bytes, static_cast<u32>(value));
    writeU32(bytes, static_cast<u32>(value >> 32U));
}
void writeFloat(std::vector<u8>& bytes, f32 value) {
    writeU32(bytes, std::bit_cast<u32>(value));
}
void writePosition(std::vector<u8>& bytes, const Vec3& position) {
    for (const f32 value : {position.x, position.y, position.z}) {
        writeFloat(bytes, value);
    }
}
u64 readU64(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
f32 readFloat(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
Vec3 readPosition(ByteReader& reader) {
    const f32 x = readFloat(reader);
    const f32 y = readFloat(reader);
    const f32 z = readFloat(reader);
    return {x, y, z};
}
bool validPosition(const Vec3& position) {
    return std::ranges::all_of(std::array{position.x, position.y, position.z}, [](f32 value) {
        return std::isfinite(value) && std::abs(value) <= 1'000'000.0f;
    });
}
bool validAngle(f32 angle) {
    // Senders normalize accumulated rotations before encoding them.
    return std::isfinite(angle) && std::abs(angle) <= kTwoPi;
}
} // namespace

bool MotionSnapshot::valid() const {
    if (epoch == 0 || cameraContinuity == 0 || !validPosition(camera.position) ||
        !validAngle(camera.pitch) || !validAngle(camera.yaw) || !validAngle(camera.roll) ||
        !std::isfinite(horizontalFov) || horizontalFov < 0.05f || horizontalFov > 3.09f ||
        !std::isfinite(aspect) || aspect < 0.25f || aspect > 8.0f) {
        return false;
    }
    return std::ranges::all_of(players, [](const auto& player) {
        return !player || (player->grant != 0 && player->continuity != 0 &&
                           validPosition(player->position) && validAngle(player->yaw));
    });
}

std::optional<std::vector<u8>> MotionPacket::encode(const MotionSnapshot& snapshot) {
    if (!snapshot.valid()) {
        return std::nullopt;
    }
    u8 mask = 0;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (snapshot.players[seat]) {
            mask |= static_cast<u8>(1U << seat);
        }
    }
    std::vector<u8> bytes;
    bytes.reserve(kMaxBytes);
    writeU32(bytes, kMagic);
    bytes.push_back(static_cast<u8>(kVersion));
    bytes.push_back(static_cast<u8>(kVersion >> 8U));
    bytes.push_back(mask);
    bytes.push_back(0);
    writeU64(bytes, snapshot.epoch);
    writeU64(bytes, snapshot.tick);
    writeU32(bytes, snapshot.cameraContinuity);
    writePosition(bytes, snapshot.camera.position);
    for (const f32 value : {snapshot.camera.pitch, snapshot.camera.yaw, snapshot.camera.roll,
                            snapshot.horizontalFov, snapshot.aspect}) {
        writeFloat(bytes, value);
    }
    for (const auto& player : snapshot.players) {
        if (player) {
            writeU32(bytes, player->grant);
            writeU32(bytes, player->continuity);
            writePosition(bytes, player->position);
            writeFloat(bytes, player->yaw);
        }
    }
    return bytes;
}

std::optional<MotionSnapshot> MotionPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU16() != kVersion) {
        return std::nullopt;
    }
    const u8 mask = reader.readU8();
    if (mask > 15 || reader.readU8() != 0 ||
        bytes.size() != kHeaderBytes + static_cast<usize>(std::popcount(mask)) * kSeatBytes) {
        return std::nullopt;
    }
    MotionSnapshot snapshot;
    snapshot.epoch = readU64(reader);
    snapshot.tick = readU64(reader);
    snapshot.cameraContinuity = reader.readU32();
    snapshot.camera.position = readPosition(reader);
    snapshot.camera.pitch = readFloat(reader);
    snapshot.camera.yaw = readFloat(reader);
    snapshot.camera.roll = readFloat(reader);
    snapshot.horizontalFov = readFloat(reader);
    snapshot.aspect = readFloat(reader);
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if ((mask & (1U << seat)) != 0) {
            SeatMotion player;
            player.grant = reader.readU32();
            player.continuity = reader.readU32();
            player.position = readPosition(reader);
            player.yaw = readFloat(reader);
            snapshot.players[seat] = player;
        }
    }
    return snapshot.valid() ? std::optional{snapshot} : std::nullopt;
}

} // namespace gdl::game
