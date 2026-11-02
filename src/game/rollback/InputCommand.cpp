#include "game/rollback/InputCommand.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLI");
static_assert(sizeof(f32) == 4 && std::numeric_limits<f32>::is_iec559);

bool validAim(const Vec3& aim) {
    const f32 length = glm::dot(aim, aim);
    return std::isfinite(aim.x) && std::isfinite(aim.z) && aim.y == 0 &&
           (length == 0 || (length >= 0.99999f && length <= 1.00001f));
}
} // namespace

namespace {
constexpr f32 kTurnSteps = 256;
constexpr f32 kMagnitudeSteps = 64;
constexpr f32 kTau = 6.283185307179586f;

/** The unit vector a turn's nearest step points along, as (cos, sin); the four axes come
 * out exact rather than a rounding error off zero. */
Vec2 steppedTurn(f32 radians) {
    const auto step = static_cast<s32>(std::round(radians / kTau * kTurnSteps));
    const s32 quarter = static_cast<s32>(kTurnSteps) / 4;
    const s32 wrapped = ((step % static_cast<s32>(kTurnSteps)) + static_cast<s32>(kTurnSteps)) %
                        static_cast<s32>(kTurnSteps);
    if (wrapped % quarter == 0) {
        constexpr std::array<Vec2, 4> kAxes{Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0}, Vec2{0, -1}};
        return kAxes[static_cast<usize>(wrapped / quarter)];
    }
    const f32 turn = static_cast<f32>(wrapped) * kTau / kTurnSteps;
    return Vec2{portable::cos(turn), portable::sin(turn)};
}
} // namespace

InputCommand InputCommand::steadied() const {
    InputCommand result = *this;
    for (usize axis = 0; axis < 3; ++axis) {
        const auto index = static_cast<glm::length_t>(axis);
        result.consoleFlight[index] =
            std::isfinite(consoleFlight[index])
                ? std::clamp(std::round(consoleFlight[index] * 127.0f), -127.0f, 127.0f) / 127.0f
                : 0.0f;
        if (result.consoleFlight[index] == 0) {
            result.consoleFlight[index] = 0.0f; // wire zero has one sign
        }
    }
    result.magnitude =
        std::clamp(std::round(magnitude * kMagnitudeSteps) / kMagnitudeSteps, 0.0f, 1.0f);
    if (result.magnitude == 0 || !std::isfinite(direction.x) || !std::isfinite(direction.y)) {
        result.direction = Vec2{0};
    } else {
        result.direction = steppedTurn(portable::atan2(direction.y, direction.x));
    }
    if (aimDirection && std::isfinite(aimDirection->x) && std::isfinite(aimDirection->z) &&
        glm::dot(*aimDirection, *aimDirection) > 0) {
        const Vec2 turned = steppedTurn(portable::atan2(aimDirection->x, aimDirection->z));
        result.aimDirection = Vec3{turned.y, 0, turned.x};
    }
    return result;
}

bool InputCommand::valid() const {
    const f32 lengthSquared = glm::dot(direction, direction);
    return epoch != 0 && grant != 0 && seat < kSeats && std::isfinite(direction.x) &&
           std::isfinite(direction.y) && std::isfinite(magnitude) && magnitude >= 0 &&
           magnitude <= 1 && lengthSquared <= 1.00001f &&
           (magnitude == 0 || lengthSquared >= 0.99999f) && (heldButtons & ~kHeldMask) == 0 &&
           (pressedButtons & ~kPressMask) == 0 && (!aimDirection || validAim(*aimDirection)) &&
           hintTopic <= 4 && (hintTopic != 0 || hintRevision == 0) &&
           (!console || console->valid()) && std::isfinite(consoleFlight.x) &&
           std::isfinite(consoleFlight.y) && std::isfinite(consoleFlight.z) &&
           glm::all(glm::lessThanEqual(glm::abs(consoleFlight), Vec3{1}));
}

std::optional<std::vector<u8>> InputPacket::encode(std::span<const InputCommand> commands) {
    if (commands.empty() || commands.size() > kMaxCommands ||
        !std::ranges::all_of(commands, &InputCommand::valid)) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + commands.size() * kCommandBytes);
    ByteWriter out(bytes);
    out.writeU32(kMagic);
    out.writeU16(kVersion);
    out.writeU8(static_cast<u8>(commands.size()));
    out.writeU8(0);
    for (const auto& command : commands) {
        out.writeU64(command.epoch);
        out.writeU64(command.tick);
        out.writeU32(command.grant);
        out.writeU8(command.seat);
        out.writeU8(command.aimDirection ? 1 : 0);
        out.writeU8(command.hintTopic);
        out.writeU8(command.hintRevision);
        const Vec3 aim = command.aimDirection.value_or(Vec3{0});
        for (const f32 value :
             {command.direction.x, command.direction.y, command.magnitude, aim.x, aim.y, aim.z}) {
            out.writeF32(value);
        }
        out.writeU32(command.heldButtons);
        out.writeU32(command.pressedButtons);
        for (const f32 value :
             {command.consoleFlight.x, command.consoleFlight.y, command.consoleFlight.z}) {
            out.writeU8(std::bit_cast<u8>(static_cast<s8>(std::round(value * 127.0f))));
        }
        out.writeU8(command.console ? static_cast<u8>(command.console->line.size()) : u8{0});
        if (command.console) {
            out.writeU8(command.console->seat);
            out.writeString(command.console->line);
        }
    }
    if (bytes.size() > kMaxBytes) {
        return std::nullopt;
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
        reader.remaining() < count * kCommandBytes) {
        return std::nullopt;
    }
    std::vector<InputCommand> commands;
    commands.reserve(count);
    for (usize i = 0; i < count; ++i) {
        if (reader.remaining() < kCommandBytes) {
            return std::nullopt;
        }
        InputCommand command;
        command.epoch = reader.readU64();
        command.tick = reader.readU64();
        command.grant = reader.readU32();
        command.seat = reader.readU8();
        const u8 aimPresent = reader.readU8();
        command.hintTopic = reader.readU8();
        command.hintRevision = reader.readU8();
        if (aimPresent > 1) {
            return std::nullopt;
        }
        command.direction.x = reader.readF32();
        command.direction.y = reader.readF32();
        command.magnitude = reader.readF32();
        Vec3 aim;
        aim.x = reader.readF32();
        aim.y = reader.readF32();
        aim.z = reader.readF32();
        if (aimPresent != 0) {
            command.aimDirection = aim;
        } else if (aim != Vec3{0}) {
            return std::nullopt;
        }
        command.heldButtons = reader.readU32();
        command.pressedButtons = reader.readU32();
        for (usize axis = 0; axis < 3; ++axis) {
            const s8 value = std::bit_cast<s8>(reader.readU8());
            if (value == std::numeric_limits<s8>::min()) {
                return std::nullopt;
            }
            command.consoleFlight[static_cast<glm::length_t>(axis)] =
                static_cast<f32>(value) / 127.0f;
        }
        const usize consoleLength = reader.readU8();
        if (consoleLength != 0) {
            if (consoleLength > ConsoleAction::kMaxLine || reader.remaining() < consoleLength + 1) {
                return std::nullopt;
            }
            ConsoleAction action;
            action.seat = reader.readU8();
            action.line = reader.readString(consoleLength);
            command.console = std::move(action);
        }
        if (!command.valid()) {
            return std::nullopt;
        }
        commands.push_back(command);
    }
    return reader.atEnd() ? std::optional{std::move(commands)} : std::nullopt;
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
