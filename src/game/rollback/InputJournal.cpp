#include "game/rollback/InputJournal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>

#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLJ");
constexpr u8 kLastFlag = 1;

bool sameControls(const InputCommand& a, const InputCommand& b) {
    const auto bits = [](f32 value) { return std::bit_cast<u32>(value); };
    const Vec3 aimA = a.aimDirection.value_or(Vec3{0});
    const Vec3 aimB = b.aimDirection.value_or(Vec3{0});
    return a.grant == b.grant && bits(a.direction.x) == bits(b.direction.x) &&
           bits(a.direction.y) == bits(b.direction.y) && bits(a.magnitude) == bits(b.magnitude) &&
           a.aimDirection.has_value() == b.aimDirection.has_value() &&
           bits(aimA.x) == bits(aimB.x) && bits(aimA.y) == bits(aimB.y) &&
           bits(aimA.z) == bits(aimB.z) && a.heldButtons == b.heldButtons &&
           a.pressedButtons == b.pressedButtons && a.hintTopic == b.hintTopic &&
           a.hintRevision == b.hintRevision && a.console == b.console &&
           bits(a.consoleFlight.x) == bits(b.consoleFlight.x) &&
           bits(a.consoleFlight.y) == bits(b.consoleFlight.y) &&
           bits(a.consoleFlight.z) == bits(b.consoleFlight.z);
}

usize runBytes(const InputCommand& command) {
    return JournalPacket::kRunBytes + (command.console ? 1 + command.console->line.size() : 0);
}

void writeRun(ByteWriter& out, const InputCommand& command, u16 run) {
    out.writeU32(command.grant);
    out.writeF32(command.direction.x);
    out.writeF32(command.direction.y);
    out.writeF32(command.magnitude);
    out.writeU8(command.aimDirection ? 1 : 0);
    const Vec3 aim = command.aimDirection.value_or(Vec3{0});
    out.writeF32(aim.x);
    out.writeF32(aim.y);
    out.writeF32(aim.z);
    out.writeU32(command.heldButtons);
    out.writeU32(command.pressedButtons);
    out.writeU8(command.hintTopic);
    out.writeU8(command.hintRevision);
    for (const f32 value :
         {command.consoleFlight.x, command.consoleFlight.y, command.consoleFlight.z}) {
        out.writeU8(std::bit_cast<u8>(static_cast<s8>(std::round(value * 127.0f))));
    }
    out.writeU8(command.console ? static_cast<u8>(command.console->line.size()) : u8{0});
    if (command.console) {
        out.writeU8(command.console->seat);
        out.writeString(command.console->line);
    }
    out.writeU16(run);
}

usize seatCount(u8 seats) {
    return static_cast<usize>(std::popcount(seats));
}

/** The number of runs each present seat's commands pack into. */
usize runCount(std::span<const InputCommand> commands, u16 count) {
    usize runs = 0;
    for (usize start = 0; start + count <= commands.size(); start += count) {
        for (usize i = 0; i < count; ++i) {
            if (i == 0 || !sameControls(commands[start + i], commands[start + i - 1])) {
                ++runs;
            }
        }
    }
    return runs;
}
} // namespace

bool EpochInputJournal::open(u64 epoch, const RollbackInputs::Owners& owners) {
    if (epoch == 0 || std::ranges::all_of(owners, [](u8 owner) { return owner == 0; })) {
        return false;
    }
    m_epoch = epoch;
    m_owners = owners;
    m_frames.clear();
    return true;
}

void EpochInputJournal::clear() {
    m_epoch = 0;
    m_owners = {};
    m_frames.clear();
}

bool EpochInputJournal::append(u64 tick, const JournalFrame& frame) {
    if (!opened() || tick != m_frames.size() + 1 || m_frames.size() >= kMaxFrames) {
        return false;
    }
    for (usize seat = 0; seat < frame.size(); ++seat) {
        const auto& command = frame[seat];
        if (m_owners[seat] == 0) {
            continue;
        }
        if (command.epoch != m_epoch || command.tick != tick || command.seat != seat ||
            !command.valid() || (command.console && m_owners[seat] != 1)) {
            return false;
        }
    }
    m_frames.push_back(frame);
    return true;
}

bool JournalPacket::Chunk::valid() const {
    if (epoch == 0 || firstTick == 0 || count == 0 || count > kMaxTicks || seats == 0 ||
        (seats & ~((1U << InputCommand::kSeats) - 1U)) != 0 ||
        commands.size() != seatCount(seats) * count) {
        return false;
    }
    usize index = 0;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        if ((seats & (1U << seat)) == 0) {
            continue;
        }
        for (u16 i = 0; i < count; ++i, ++index) {
            const auto& command = commands[index];
            if (command.epoch != epoch || command.tick != firstTick + i || command.seat != seat ||
                !command.valid()) {
                return false;
            }
        }
    }
    return true;
}

bool JournalPacket::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}

std::optional<std::vector<u8>> JournalPacket::encode(const Chunk& chunk) {
    if (!chunk.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + runCount(chunk.commands, chunk.count) * kRunBytes);
    ByteWriter out(bytes);
    out.writeU32(kMagic);
    out.writeU8(kVersion);
    out.writeU8(chunk.last ? kLastFlag : u8{0});
    out.writeU8(chunk.seats);
    out.writeU8(0);
    out.writeU64(chunk.epoch);
    out.writeU64(chunk.firstTick);
    out.writeU16(chunk.count);
    out.writeU16(0);
    for (usize start = 0; start < chunk.commands.size(); start += chunk.count) {
        usize i = 0;
        while (i < chunk.count) {
            usize run = 1;
            while (i + run < chunk.count &&
                   sameControls(chunk.commands[start + i + run], chunk.commands[start + i])) {
                ++run;
            }
            writeRun(out, chunk.commands[start + i], static_cast<u16>(run));
            i += run;
        }
    }
    if (bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<JournalPacket::Chunk> JournalPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes || !recognizes(bytes)) {
        return std::nullopt;
    }
    try {
        ByteReader reader(bytes);
        reader.skip(4);
        if (reader.readU8() != kVersion) {
            return std::nullopt;
        }
        const u8 flags = reader.readU8();
        Chunk chunk;
        chunk.seats = reader.readU8();
        if (reader.readU8() != 0 || (flags & ~kLastFlag) != 0) {
            return std::nullopt;
        }
        chunk.last = (flags & kLastFlag) != 0;
        chunk.epoch = reader.readU64();
        chunk.firstTick = reader.readU64();
        chunk.count = reader.readU16();
        if (reader.readU16() != 0 || chunk.count == 0 || chunk.count > kMaxTicks ||
            chunk.seats == 0) {
            return std::nullopt;
        }
        chunk.commands.reserve(seatCount(chunk.seats) * chunk.count);
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((chunk.seats & (1U << seat)) == 0) {
                continue;
            }
            u16 covered = 0;
            while (covered < chunk.count) {
                InputCommand command;
                command.epoch = chunk.epoch;
                command.seat = seat;
                command.grant = reader.readU32();
                command.direction.x = reader.readF32();
                command.direction.y = reader.readF32();
                command.magnitude = reader.readF32();
                const u8 aimPresent = reader.readU8();
                Vec3 aim;
                aim.x = reader.readF32();
                aim.y = reader.readF32();
                aim.z = reader.readF32();
                if (aimPresent > 1 || (aimPresent == 0 && aim != Vec3{0})) {
                    return std::nullopt;
                }
                if (aimPresent != 0) {
                    command.aimDirection = aim;
                }
                command.heldButtons = reader.readU32();
                command.pressedButtons = reader.readU32();
                command.hintTopic = reader.readU8();
                command.hintRevision = reader.readU8();
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
                    if (consoleLength > ConsoleAction::kMaxLine) {
                        return std::nullopt;
                    }
                    ConsoleAction action;
                    action.seat = reader.readU8();
                    action.line = reader.readString(consoleLength);
                    command.console = std::move(action);
                }
                const u16 run = reader.readU16();
                if (run == 0 || covered + run > chunk.count) {
                    return std::nullopt;
                }
                for (u16 i = 0; i < run; ++i) {
                    command.tick = chunk.firstTick + covered + i;
                    chunk.commands.push_back(command);
                }
                covered = static_cast<u16>(covered + run);
            }
        }
        if (!reader.atEnd() || !chunk.valid()) {
            return std::nullopt;
        }
        return chunk;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<JournalPacket::Chunk> JournalPacket::chunks(const EpochInputJournal& journal,
                                                        u64 firstTick, u64 lastTick,
                                                        bool markLast) {
    std::vector<Chunk> result;
    if (!journal.opened() || firstTick == 0 || lastTick < firstTick ||
        lastTick > journal.through()) {
        return result;
    }
    u8 seats = 0;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        if (journal.owners()[seat] != 0) {
            seats |= static_cast<u8>(1U << seat);
        }
    }
    u64 start = firstTick;
    while (start <= lastTick) {
        // Grow the chunk a tick at a time; a tick costs a run only where a seat changed.
        usize bytes = kHeaderBytes;
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((seats & (1U << seat)) != 0) {
                bytes += runBytes((*journal.frame(start))[seat]);
            }
        }
        u64 end = start;
        while (end < lastTick && end - start + 1 < kMaxTicks) {
            usize added = 0;
            const JournalFrame& previous = *journal.frame(end);
            const JournalFrame& next = *journal.frame(end + 1);
            for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
                if ((seats & (1U << seat)) != 0 && !sameControls(next[seat], previous[seat])) {
                    added += runBytes(next[seat]);
                }
            }
            if (bytes + added > kMaxBytes) {
                break;
            }
            bytes += added;
            ++end;
        }
        Chunk chunk;
        chunk.epoch = journal.epoch();
        chunk.firstTick = start;
        chunk.count = static_cast<u16>(end - start + 1);
        chunk.seats = seats;
        chunk.last = markLast && end == lastTick;
        for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((seats & (1U << seat)) == 0) {
                continue;
            }
            for (u64 tick = start; tick <= end; ++tick) {
                chunk.commands.push_back((*journal.frame(tick))[seat]);
            }
        }
        result.push_back(std::move(chunk));
        start = end + 1;
    }
    return result;
}

std::vector<JournalPacket::Chunk>
JournalPacket::tailChunks(u64 epoch, std::vector<InputCommand> commands, bool markLast) {
    std::vector<Chunk> result;
    std::erase_if(commands, [epoch](const InputCommand& command) {
        return command.epoch != epoch || !command.valid() || command.tick == 0;
    });
    std::ranges::sort(commands, [](const InputCommand& a, const InputCommand& b) {
        return a.seat != b.seat ? a.seat < b.seat : a.tick < b.tick;
    });
    usize start = 0;
    while (start < commands.size()) {
        usize end = start + 1;
        usize bytes = kHeaderBytes + runBytes(commands[start]);
        while (end < commands.size() && end - start < kMaxTicks &&
               commands[end].seat == commands[start].seat &&
               commands[end].tick == commands[end - 1].tick + 1) {
            const usize added =
                sameControls(commands[end], commands[end - 1]) ? 0 : runBytes(commands[end]);
            if (bytes + added > kMaxBytes) {
                break;
            }
            bytes += added;
            ++end;
        }
        Chunk chunk;
        chunk.epoch = epoch;
        chunk.firstTick = commands[start].tick;
        chunk.count = static_cast<u16>(end - start);
        chunk.seats = static_cast<u8>(1U << commands[start].seat);
        chunk.commands.assign(commands.begin() + static_cast<std::ptrdiff_t>(start),
                              commands.begin() + static_cast<std::ptrdiff_t>(end));
        result.push_back(std::move(chunk));
        start = end;
    }
    if (markLast && !result.empty()) {
        result.back().last = true;
    }
    return result;
}

} // namespace gdl::game
