#include "game/netplay/InputCommand.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLI");
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

u64 readU64(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}

bool finiteCoordinate(f32 value) {
    return std::isfinite(value) && std::abs(value) <= 1'000'000.0f;
}
} // namespace

bool InputCommand::valid() const {
    const f32 lengthSquared = glm::dot(direction, direction);
    return epoch != 0 && grant != 0 && seat < kSeats && std::isfinite(direction.x) &&
           std::isfinite(direction.y) && std::isfinite(magnitude) && magnitude >= 0 &&
           magnitude <= 1 && lengthSquared <= 1.00001f &&
           (magnitude == 0 || lengthSquared >= 0.99999f) && (heldButtons & ~kHeldMask) == 0 &&
           (pressedButtons & ~kPressMask) == 0 &&
           (!aimPoint || (finiteCoordinate(aimPoint->x) && finiteCoordinate(aimPoint->y) &&
                          finiteCoordinate(aimPoint->z)));
}

std::optional<std::vector<u8>> InputPacket::encode(std::span<const InputCommand> commands) {
    if (commands.empty() || commands.size() > kMaxCommands ||
        !std::ranges::all_of(commands, &InputCommand::valid)) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + commands.size() * kCommandBytes);
    writeU32(bytes, kMagic);
    bytes.push_back(static_cast<u8>(kVersion));
    bytes.push_back(static_cast<u8>(kVersion >> 8U));
    bytes.push_back(static_cast<u8>(commands.size()));
    bytes.push_back(0);
    for (const auto& command : commands) {
        writeU64(bytes, command.epoch);
        writeU64(bytes, command.tick);
        writeU32(bytes, command.grant);
        bytes.push_back(command.seat);
        bytes.push_back(command.aimPoint ? 1 : 0);
        bytes.push_back(0);
        bytes.push_back(0);
        const Vec3 aim = command.aimPoint.value_or(Vec3{0});
        for (const f32 value :
             {command.direction.x, command.direction.y, command.magnitude, aim.x, aim.y, aim.z}) {
            writeU32(bytes, std::bit_cast<u32>(value));
        }
        writeU32(bytes, command.heldButtons);
        writeU32(bytes, command.pressedButtons);
    }
    return bytes;
}

std::optional<std::vector<InputCommand>> InputPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU16() != kVersion) {
        return std::nullopt;
    }
    const usize count = reader.readU8();
    if (reader.readU8() != 0 || count == 0 || count > kMaxCommands ||
        reader.remaining() != count * kCommandBytes) {
        return std::nullopt;
    }
    std::vector<InputCommand> commands;
    commands.reserve(count);
    for (usize i = 0; i < count; ++i) {
        InputCommand command;
        command.epoch = readU64(reader);
        command.tick = readU64(reader);
        command.grant = reader.readU32();
        command.seat = reader.readU8();
        const u8 aimPresent = reader.readU8();
        if (aimPresent > 1 || reader.readU16() != 0) {
            return std::nullopt;
        }
        command.direction.x = std::bit_cast<f32>(reader.readU32());
        command.direction.y = std::bit_cast<f32>(reader.readU32());
        command.magnitude = std::bit_cast<f32>(reader.readU32());
        Vec3 aim;
        aim.x = std::bit_cast<f32>(reader.readU32());
        aim.y = std::bit_cast<f32>(reader.readU32());
        aim.z = std::bit_cast<f32>(reader.readU32());
        if (aimPresent != 0) {
            command.aimPoint = aim;
        } else if (aim != Vec3{0}) {
            return std::nullopt;
        }
        command.heldButtons = reader.readU32();
        command.pressedButtons = reader.readU32();
        if (!command.valid()) {
            return std::nullopt;
        }
        commands.push_back(command);
    }
    return commands;
}

bool InputHistory::record(std::span<const InputCommand> frame) {
    if (frame.empty() || frame.size() > InputCommand::kSeats) {
        return false;
    }
    const auto& latest = frame.front();
    if (!m_commands.empty() &&
        (latest.epoch < m_commands.back().epoch ||
         (latest.epoch == m_commands.back().epoch && latest.tick <= m_commands.back().tick))) {
        return false;
    }
    std::array<u32, InputCommand::kSeats> grants{};
    for (const auto& command : frame) {
        if (!command.valid() || command.epoch != latest.epoch || command.tick != latest.tick ||
            grants[command.seat] != 0) {
            return false;
        }
        grants[command.seat] = command.grant;
    }
    std::erase_if(m_commands, [&](const auto& command) {
        return command.epoch != latest.epoch || grants[command.seat] != command.grant ||
               latest.tick - command.tick >= kTicks;
    });
    m_commands.insert(m_commands.end(), frame.begin(), frame.end());
    return true;
}

} // namespace gdl::game
