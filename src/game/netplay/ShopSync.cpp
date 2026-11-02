#include "game/netplay/ShopSync.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"
#include "engine/math/RandomDistribution.h"

#include "game/rollback/InputCommand.h"

namespace gdl::game {
namespace {
/** Every seat of the party, the only ones a setup may name as fallen. */
constexpr SeatSet kSeats = [] {
    SeatSet seats;
    for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
        seats.insert(seat);
    }
    return seats;
}();
/** The setup packet's flag word: two bits, then a bit a seat for the fallen. */
constexpr u32 kSetupExpanded = 0x1;
constexpr u32 kSetupEnding = 0x2;
constexpr u32 kSetupFallenShift = 8;
constexpr u32 kSetupMask = kSetupExpanded | kSetupEnding | (0xFU << kSetupFallenShift);

constexpr u32 kMagic = fourcc("GDSH");
constexpr u8 kVersion = 2;
enum class Kind : u8 { Setup, Input, Step, Done };
constexpr u16 kHeld = 0x780;
constexpr u16 kActions = 0x7ff;
/** The header every shop packet opens with: magic, version, kind, two zero bytes and epoch. */
std::vector<u8> packet(u64 epoch, Kind kind) {
    std::vector<u8> bytes;
    ByteWriter out(bytes);
    out.writeU32(kMagic);
    out.writeU8(kVersion);
    out.writeU8(static_cast<u8>(kind));
    out.writeU16(0);
    out.writeU64(epoch);
    return bytes;
}
u16 actions(const MenuInput& input) {
    const std::array values{input.up,       input.down,     input.left,     input.right,
                            input.select,   input.back,     input.start,    input.upHeld,
                            input.downHeld, input.leftHeld, input.rightHeld};
    u16 bits = 0;
    for (usize i = 0; i < values.size(); ++i) {
        bits |= static_cast<u16>(static_cast<u16>(values[i]) << i);
    }
    return bits;
}
MenuInput menu(u16 bits) {
    MenuInput input;
    const std::array fields{&input.up,       &input.down,     &input.left,     &input.right,
                            &input.select,   &input.back,     &input.start,    &input.upHeld,
                            &input.downHeld, &input.leftHeld, &input.rightHeld};
    for (usize i = 0; i < fields.size(); ++i) {
        *fields[i] = (bits & (1U << i)) != 0;
    }
    return input;
}
} // namespace
bool ShopSetup::valid() const {
    return source != 0 && source <= 65535 && destination != 0 && destination <= 65535 &&
           kSeats.includes(fallen) && std::ranges::all_of(maxima, [](s32 v) { return v >= 0; }) &&
           std::ranges::all_of(totals, [](const auto& values) {
               return std::ranges::all_of(values, [](s32 v) { return v >= 0; });
           });
}
bool ShopSync::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}
bool ShopSync::broadcast(const std::vector<u8>& bytes) {
    usize peers = 0;
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (std::ranges::find(m_owners, peer) != m_owners.end()) {
            ++peers;
        }
    }
    if (m_outgoing.size() + peers > kBudget) {
        return false;
    }
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (std::ranges::find(m_owners, peer) != m_owners.end()) {
            m_outgoing.push_back({peer, bytes});
        }
    }
    return true;
}
void ShopSync::begin(u64 epoch, u8 local, const std::array<u8, 4>& owners,
                     const std::array<u32, 4>& grants, const ShopSetup* setup) {
    m_epoch = epoch;
    m_local = local;
    m_owners = owners;
    m_grants = grants;
    m_setup.reset();
    m_sent = {};
    m_received = {};
    m_last = {};
    m_held = {};
    m_inputs = {};
    m_finished = {};
    m_steps.clear();
    m_outgoing.clear();
    m_tick = 0;
    if (local == 1 && setup != nullptr && setup->valid()) {
        m_setup = *setup;
        auto bytes = packet(epoch, Kind::Setup);
        ByteWriter out(bytes);
        out.writeU32(setup->source);
        out.writeU32(setup->destination);
        out.writeU32((setup->expanded ? kSetupExpanded : 0U) | (setup->ending ? kSetupEnding : 0U) |
                     (static_cast<u32>(setup->fallen.bits()) << kSetupFallenShift));
        for (const auto value : setup->maxima) {
            out.writeS32(value);
        }
        for (const auto& totals : setup->totals) {
            for (const auto value : totals) {
                out.writeS32(value);
            }
        }
        broadcast(bytes);
    }
}
ShopSync::Admission ShopSync::receive(u8 peer, std::span<const u8> bytes) {
    if (bytes.size() < 16 || !recognizes(bytes)) {
        return Admission::Invalid;
    }
    ByteReader reader(bytes);
    reader.readU32();
    if (reader.readU8() != kVersion) {
        return Admission::Invalid;
    }
    const auto kind = static_cast<Kind>(reader.readU8());
    if (reader.readU16() != 0) {
        return Admission::Invalid;
    }
    const auto epoch = reader.readU64();
    if (epoch != m_epoch) {
        return epoch < m_epoch ? Admission::Stale : Admission::Invalid;
    }
    if (peer == 0 || peer > 4 || peer == m_local ||
        std::ranges::find(m_owners, peer) == m_owners.end()) {
        return Admission::WrongPeer;
    }
    if (kind == Kind::Setup) {
        if (m_local == 1 || peer != 1) {
            return Admission::WrongPeer;
        }
        if (bytes.size() != 88 || m_setup) {
            return Admission::Invalid;
        }
        ShopSetup setup;
        setup.source = reader.readU32();
        setup.destination = reader.readU32();
        const auto flags = reader.readU32();
        if ((flags & ~kSetupMask) != 0) {
            return Admission::Invalid;
        }
        setup.expanded = (flags & kSetupExpanded) != 0;
        setup.ending = (flags & kSetupEnding) != 0;
        setup.fallen = SeatSet::fromBits(static_cast<u8>(flags >> kSetupFallenShift));
        for (auto& value : setup.maxima) {
            value = reader.readS32();
        }
        for (auto& totals : setup.totals) {
            for (auto& value : totals) {
                value = reader.readS32();
            }
        }
        if (!setup.valid()) {
            return Admission::Invalid;
        }
        m_setup = setup;
        return Admission::Accepted;
    }
    if (!m_setup) {
        return Admission::Invalid;
    }
    if (kind == Kind::Input) {
        if (m_local != 1 || bytes.size() != 48) {
            return Admission::WrongPeer;
        }
        const auto seat = reader.readU32();
        const auto grant = reader.readU32();
        const auto sequence = reader.readU64();
        const auto bits = reader.readU32();
        const auto focus = reader.readS32();
        const auto phase = reader.readU32();
        const auto cursor = reader.readU32();
        if (focus < -1 || focus > 255 || phase > 6 || cursor > 255 || seat >= 4 ||
            (bits & ~kActions) != 0) {
            return Admission::Invalid;
        }
        if (m_owners[seat] != peer || m_grants[seat] != grant) {
            return Admission::WrongPeer;
        }
        if (sequence <= m_received[seat]) {
            return Admission::Stale;
        }
        if (sequence != m_received[seat] + 1 ||
            !hold(seat, {static_cast<u16>(bits), focus, static_cast<u8>(phase),
                         static_cast<u16>(cursor)})) {
            return Admission::Invalid;
        }
        m_received[seat] = sequence;
        return Admission::Accepted;
    }
    if (kind == Kind::Step) {
        if (m_local == 1 || peer != 1 || bytes.size() != 72) {
            return Admission::WrongPeer;
        }
        ShopStep step;
        step.tick = reader.readU64();
        if (step.tick < m_tick) {
            return Admission::Stale;
        }
        if (step.tick != m_tick) {
            return Admission::Invalid;
        }
        for (usize seat = 0; seat < 4; ++seat) {
            const auto bits = reader.readU32();
            const auto potion = reader.readU32();
            const auto focus = reader.readS32();
            if ((bits & ~kActions) != 0 || potion < 1 || potion > 4 || focus < -1 || focus > 255 ||
                (m_owners[seat] == 0 && (bits != 0 || focus != -1))) {
                return Admission::Invalid;
            }
            step.inputs[seat] = menu(static_cast<u16>(bits));
            step.potions[seat] = static_cast<s32>(potion);
            step.focus[seat] = focus;
        }
        m_steps.push_back(std::move(step));
        ++m_tick;
        return Admission::Accepted;
    }
    if (kind == Kind::Done && m_local == 1 && bytes.size() == 24) {
        if (reader.readU64() > m_tick) {
            return Admission::Invalid;
        }
        m_finished[peer] = true;
        return Admission::Accepted;
    }
    return Admission::Invalid;
}
bool ShopSync::sample(u8 seat, const MenuInput& input, s32 focus, u8 observedPhase,
                      usize observedCursor) {
    if (!m_setup || seat >= 4 || m_owners[seat] != m_local || focus < -1 || focus > 255 ||
        observedPhase > 6 || observedCursor > 255) {
        return false;
    }
    const auto bits = actions(input);
    if (bits == m_last[seat] && (bits & ~kHeld) == 0 && focus == -1) {
        return true;
    }
    if (m_sent[seat] == std::numeric_limits<u64>::max()) {
        return false;
    }
    if (m_local == 1) {
        // The host's own commands wait as a guest's do, folded alike.
        if (!hold(seat, {bits, focus, observedPhase, static_cast<u16>(observedCursor)})) {
            return false;
        }
    } else {
        if (m_outgoing.size() >= kBudget) {
            return false;
        }
        auto bytes = packet(m_epoch, Kind::Input);
        ByteWriter out(bytes);
        out.writeU32(seat);
        out.writeU32(m_grants[seat]);
        out.writeU64(m_sent[seat] + 1);
        out.writeU32(bits);
        out.writeS32(focus);
        out.writeU32(observedPhase);
        out.writeU32(static_cast<u32>(observedCursor));
        m_outgoing.push_back({1, std::move(bytes)});
    }
    ++m_sent[seat];
    m_last[seat] = bits;
    return true;
}
bool ShopSync::hold(usize seat, const Command& command) {
    auto& waiting = m_inputs[seat];
    const auto pressFree = [](const Command& queued) { return (queued.bits & ~kHeld) == 0; };
    if (!waiting.empty() && pressFree(waiting.back()) && pressFree(command)) {
        const s32 focus = command.focus != -1 ? command.focus : waiting.back().focus;
        waiting.back() = command;
        waiting.back().focus = focus;
        return true;
    }
    // Only presses count: press-free commands fold, so one at most waits between two presses.
    const auto presses =
        std::ranges::count_if(waiting, [&](const Command& queued) { return !pressFree(queued); });
    if (!pressFree(command) && std::cmp_greater_equal(presses, kHeldPresses)) {
        return false;
    }
    waiting.push_back(command);
    return true;
}
std::optional<ShopStep> ShopSync::advance() {
    if (m_local != 1 || !m_setup || m_outgoing.size() + 3 > kBudget ||
        m_tick == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    ShopStep step;
    step.tick = m_tick;
    for (usize seat = 0; seat < 4; ++seat) {
        u16 bits = m_held[seat];
        if (!m_inputs[seat].empty()) {
            bits = m_inputs[seat].front().bits;
            step.focus[seat] = m_inputs[seat].front().focus;
            step.observedPhase[seat] = m_inputs[seat].front().phase;
            step.observedCursor[seat] = m_inputs[seat].front().cursor;
            m_inputs[seat].pop_front();
        }
        m_held[seat] = bits & kHeld;
        step.inputs[seat] = menu(bits);
        // This remains a host-authored outcome, not shared lockstep RNG state.
        // Preserve all four draws even for idle or unowned seats.
        step.potions[seat] =
            1 + static_cast<s32>(random_distribution::boundedIndex(m_random, 4).value());
    }
    return step;
}
bool ShopSync::publish(const ShopStep& step) {
    if (m_local != 1 || step.tick != m_tick) {
        return false;
    }
    auto bytes = packet(m_epoch, Kind::Step);
    ByteWriter out(bytes);
    out.writeU64(step.tick);
    for (usize seat = 0; seat < 4; ++seat) {
        out.writeU32(actions(step.inputs[seat]));
        out.writeS32(step.potions[seat]);
        out.writeS32(step.focus[seat]);
    }
    if (!broadcast(bytes)) {
        return false;
    }
    ++m_tick;
    return true;
}
std::optional<ShopStep> ShopSync::take() {
    if (m_steps.empty()) {
        return std::nullopt;
    }
    auto step = std::move(m_steps.front());
    m_steps.pop_front();
    return step;
}
bool ShopSync::finish() {
    if (!m_setup || m_local == 0) {
        return false;
    }
    if (m_finished[m_local]) {
        return true;
    }
    if (m_outgoing.size() >= kBudget) {
        return false;
    }
    if (m_local != 1) {
        auto bytes = packet(m_epoch, Kind::Done);
        ByteWriter(bytes).writeU64(m_tick);
        m_outgoing.push_back({1, std::move(bytes)});
    }
    m_finished[m_local] = true;
    return true;
}
void ShopSync::suspend(u8 peer) {
    if (peer != 0 && peer < m_finished.size()) {
        m_finished[peer] = true;
    }
}
bool ShopSync::finished() const {
    return m_local == 1 && m_setup &&
           std::ranges::all_of(m_owners, [this](u8 peer) { return peer == 0 || m_finished[peer]; });
}
} // namespace gdl::game
