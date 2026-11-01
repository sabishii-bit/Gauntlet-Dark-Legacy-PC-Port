#include "game/netplay/RollbackExchange.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDRI");
void write(std::vector<u8>& bytes, u64 value, usize count) {
    for (usize i = 0; i < count; ++i) {
        bytes.push_back(static_cast<u8>(value >> (i * 8)));
    }
}
u64 read64(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32);
}
bool same(std::span<const InputCommand> a, std::span<const InputCommand> b) {
    return InputPacket::encode(a) == InputPacket::encode(b);
}
bool tooFar(u64 value, u64 base) {
    return value > base && value - base > RollbackInputs::kMaxAhead;
}
u64 steadyMicroseconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
u32 boundedAge(u64 age) {
    return static_cast<u32>(std::min<u64>(age, RollbackExchange::kMaxProgressAgeUs));
}
u32 addAge(u32 age, u64 elapsed) {
    return age + boundedAge(std::min<u64>(elapsed, RollbackExchange::kMaxProgressAgeUs - age));
}
u32 linkAge(s32 pingMs) {
    return pingMs < 0 ? RollbackExchange::kMaxProgressAgeUs
                      : boundedAge(static_cast<u64>(pingMs) * 500);
}
u8 ageTicks(u32 age) {
    return static_cast<u8>(std::min<u64>(RollbackInputs::kPredictionTicks,
                                         (static_cast<u64>(age) * 60 + 999'999) / 1'000'000));
}
} // namespace

std::optional<std::vector<u8>> RollbackExchange::encode(const Message& message) {
    if (message.epoch == 0 || message.origin == 0 || message.origin > 4 || message.nextTick == 0 ||
        message.received[0] != 0 || message.progressSerial == 0 ||
        message.progressAgeUs > kMaxProgressAgeUs) {
        return std::nullopt;
    }
    const auto inputs = message.commands.empty() ? std::optional{std::vector<u8>{}}
                                                 : InputPacket::encode(message.commands);
    if (!inputs || inputs->size() > kMaxBytes - kHeaderBytes ||
        std::ranges::any_of(message.commands, [&](const auto& command) {
            return command.epoch != message.epoch || command.tick == 0;
        })) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes + inputs->size());
    write(bytes, kMagic, 4);
    write(bytes, kVersion, 1);
    write(bytes, message.origin, 1);
    write(bytes, 0, 2);
    write(bytes, message.epoch, 8);
    write(bytes, message.nextTick, 8);
    for (usize peer = 1; peer <= 4; ++peer) {
        write(bytes, message.received[peer], 8);
    }
    write(bytes, message.progressSerial, 8);
    write(bytes, message.progressAgeUs, 4);
    bytes.insert(bytes.end(), inputs->begin(), inputs->end());
    return bytes;
}
bool RollbackExchange::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && readU32LE(bytes, 0) == kMagic;
}
std::optional<RollbackExchange::Message> RollbackExchange::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes || !recognizes(bytes)) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    reader.skip(4);
    if (reader.readU8() != kVersion) {
        return std::nullopt;
    }
    Message message;
    message.origin = reader.readU8();
    if (reader.readU16() != 0) {
        return std::nullopt;
    }
    message.epoch = read64(reader);
    message.nextTick = read64(reader);
    for (usize peer = 1; peer <= 4; ++peer) {
        message.received[peer] = read64(reader);
    }
    message.progressSerial = read64(reader);
    message.progressAgeUs = reader.readU32();
    if (!reader.atEnd()) {
        auto commands = InputPacket::decode(reader.readBytes(reader.remaining()));
        if (!commands) {
            return std::nullopt;
        }
        message.commands = std::move(*commands);
    }
    return encode(message) ? std::optional{std::move(message)} : std::nullopt;
}

bool RollbackExchange::occupied(u8 peer) const {
    return peer > 0 && peer <= 4 && std::ranges::find(m_owners, peer) != m_owners.end();
}
bool RollbackExchange::open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants,
                            u8 delay, std::span<const Link> links, Clock clock,
                            std::span<const u8> absent) {
    if (m_open || localPeer == 0 || localPeer > 4 || epoch == 0 ||
        delay > RollbackInputs::kMaxDelay || std::ranges::find(owners, localPeer) == owners.end() ||
        std::ranges::find(owners, 1) == owners.end()) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] > 4 || (owners[seat] == 0) != (grants[seat] == 0)) {
            return false;
        }
    }
    std::array<PacketTransport::Connection, 5> connections{};
    std::set<PacketTransport::Connection> seen;
    for (const auto& link : links) {
        if (link.peer == 0 || link.peer > 4 || link.peer == localPeer || link.connection == 0 ||
            connections[link.peer] != 0 || !seen.insert(link.connection).second ||
            std::ranges::find(owners, link.peer) == owners.end()) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    std::array<bool, 5> gone{};
    for (const u8 peer : absent) {
        if (peer < 2 || peer > 4 || peer == localPeer || connections[peer] != 0 ||
            std::ranges::find(owners, peer) == owners.end()) {
            return false;
        }
        gone[peer] = true;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        const bool needed = peer != localPeer && std::ranges::find(owners, peer) != owners.end() &&
                            (localPeer == 1 || peer == 1) && !gone[peer];
        // The star is required; a guest may add direct links, the host has nothing to add.
        if ((needed && connections[peer] == 0) ||
            (!needed && connections[peer] != 0 && localPeer == 1)) {
            return false;
        }
    }
    m_open = true;
    m_local = localPeer;
    m_epoch = epoch;
    m_delay = delay;
    m_owners = owners;
    m_grants = grants;
    m_links = connections;
    m_suspended = gone;
    m_clock = clock ? std::move(clock) : Clock{steadyMicroseconds};
    m_lastTime = m_clock();
    m_progress[m_local] = {1, 0, m_lastTime};
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (!occupied(peer)) {
            continue;
        }
        m_next[peer] = 1;
        m_direct[peer] = localPeer != 1 && peer != 1 && connections[peer] != 0;
        for (u8 origin = 1; origin <= 4; ++origin) {
            m_received[peer][origin] = occupied(origin) ? delay : 0;
        }
    }
    return true;
}
std::optional<u64> RollbackExchange::now() {
    const u64 value = m_clock();
    if (value < m_lastTime) {
        fail(Failure::Transport);
        return std::nullopt;
    }
    m_lastTime = value;
    return value;
}
RollbackExchange::Admission RollbackExchange::validate(const Message& message,
                                                       History& incoming) const {
    if (message.epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    if (!occupied(message.origin) || message.nextTick == 0 || message.received[0] != 0 ||
        tooFar(message.nextTick, m_next[m_local]) ||
        message.commands.size() > RollbackInputs::kRedundantTicks * InputCommand::kSeats) {
        return Admission::Invalid;
    }
    const auto serial = m_progress[message.origin].serial;
    if (message.progressSerial == serial && message.nextTick != m_next[message.origin]) {
        return Admission::Conflict;
    }
    if ((message.progressSerial > serial && message.nextTick < m_next[message.origin]) ||
        (message.progressSerial < serial && message.nextTick > m_next[message.origin])) {
        return Admission::Invalid;
    }
    for (u8 origin = 1; origin <= 4; ++origin) {
        if ((!occupied(origin) && message.received[origin] != 0) ||
            tooFar(message.received[origin], m_next[m_local]) ||
            (origin == m_local && message.received[origin] > m_received[m_local][m_local])) {
            return Admission::Invalid;
        }
    }
    for (const auto& command : message.commands) {
        if (!command.valid() || command.epoch != m_epoch || command.tick <= m_delay ||
            command.grant != m_grants[command.seat] || m_owners[command.seat] != message.origin ||
            tooFar(command.tick, m_next[m_local]) ||
            (command.console && (message.origin != 1 || m_owners[command.console->seat] == 0))) {
            return Admission::Invalid;
        }
        incoming[command.tick].push_back(command);
    }
    if (incoming.size() > RollbackInputs::kRedundantTicks) {
        return Admission::Invalid;
    }
    const auto& history = m_history[message.origin];
    usize added = 0;
    for (auto& [tick, frame] : incoming) {
        std::ranges::sort(frame, {}, &InputCommand::seat);
        std::array<bool, InputCommand::kSeats> seats{};
        for (const auto& command : frame) {
            if (seats[command.seat]) {
                return Admission::Invalid;
            }
            seats[command.seat] = true;
        }
        for (usize seat = 0; seat < seats.size(); ++seat) {
            if (seats[seat] != (m_owners[seat] == message.origin)) {
                return Admission::Invalid;
            }
        }
        const auto found = history.find(tick);
        if (found != history.end()) {
            if (!same(frame, found->second)) {
                return Admission::Conflict;
            }
        } else if (tick > m_received[m_local][message.origin]) {
            ++added;
        }
    }
    return history.size() + added > kHistory ? Admission::Full : Admission::Accepted;
}
void RollbackExchange::commit(const Message& message, const History& incoming,
                              std::optional<Progress> progress) {
    const u8 origin = message.origin;
    auto& history = m_history[origin];
    auto& through = m_received[m_local][origin];
    for (const auto& [tick, frame] : incoming) {
        if (tick > through) {
            history.try_emplace(tick, frame);
        }
    }
    while (through != std::numeric_limits<u64>::max() && history.contains(through + 1)) {
        ++through;
    }
    if (progress) {
        m_next[origin] = message.nextTick;
        m_progress[origin] = *progress;
    }
    if (origin != m_local) {
        for (u8 source = 1; source <= 4; ++source) {
            m_received[origin][source] =
                std::max(m_received[origin][source], message.received[source]);
        }
    }
    trim();
}
void RollbackExchange::trim() {
    for (u8 origin = 1; origin <= 4; ++origin) {
        auto& history = m_history[origin];
        if (history.empty()) {
            continue;
        }
        u64 acknowledged = m_received[m_local][origin];
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (occupied(peer) && !m_suspended[peer]) {
                acknowledged = std::min(acknowledged, m_received[peer][origin]);
            }
        }
        const u64 newest = history.rbegin()->first;
        const u64 keepFrom = newest >= RollbackInputs::kRedundantTicks
                                 ? newest - RollbackInputs::kRedundantTicks + 1
                                 : 1;
        while (!history.empty() && history.begin()->first <= acknowledged &&
               history.begin()->first < keepFrom) {
            history.erase(history.begin());
        }
    }
}
bool RollbackExchange::publish(std::span<const InputCommand> commands, u64 nextTick) {
    if (!m_open || m_failure != Failure::None || nextTick == 0) {
        return false;
    }
    const auto time = now();
    if (!time || m_progress[m_local].serial == std::numeric_limits<u64>::max()) {
        return fail(Failure::Transport);
    }
    // Corrections may end at an earlier terminal. Pacing advertises the reached
    // high-water mark, never invents a backwards-moving progress report.
    const Message message{m_epoch,
                          m_local,
                          std::max(nextTick, m_next[m_local]),
                          m_received[m_local],
                          {commands.begin(), commands.end()},
                          m_progress[m_local].serial + 1,
                          0};
    History incoming;
    const auto result = validate(message, incoming);
    if (result != Admission::Accepted) {
        if (result == Admission::Conflict) {
            return fail(Failure::Conflict);
        }
        return result == Admission::Full ? fail(Failure::Window) : false;
    }
    commit(message, incoming, Progress{message.progressSerial, 0, *time});
    return true;
}
RollbackExchange::Admission
RollbackExchange::receive(PacketTransport::Connection connection, std::span<const u8> bytes,
                          s32 pingMs,
                          const std::function<bool(const Message&, std::optional<u8>)>& consume) {
    if (!m_open || m_failure != Failure::None || connection == 0) {
        return Admission::WrongPeer;
    }
    // An array iterator is a pointer only on some libraries:
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto source = std::ranges::find(m_links, connection);
    if (source == m_links.end()) {
        return Admission::WrongPeer;
    }
    const auto message = decode(bytes);
    if (!message) {
        return Admission::Invalid;
    }
    const auto sender = static_cast<u8>(source - m_links.begin());
    // Only the host relays other origins; a guest, direct or not, sends its own.
    if (message->origin == m_local || (sender != 1 && message->origin != sender)) {
        return Admission::WrongPeer;
    }
    History incoming;
    const auto result = validate(*message, incoming);
    if (result != Admission::Accepted) {
        if (result == Admission::Conflict) {
            fail(Failure::Conflict);
        } else if (result == Admission::Full) {
            fail(Failure::Window);
        }
        return result;
    }
    const auto time = now();
    if (!time) {
        return Admission::Rejected;
    }
    std::optional<Progress> progress;
    std::optional<u8> transit;
    if (message->progressSerial > m_progress[message->origin].serial) {
        progress = Progress{message->progressSerial,
                            addAge(message->progressAgeUs, linkAge(pingMs)), *time};
        transit = ageTicks(progress->ageUs);
    }
    if (!consume || !consume(*message, transit)) {
        return Admission::Rejected;
    }
    commit(*message, incoming, progress);
    return Admission::Accepted;
}
bool RollbackExchange::flush(PacketTransport& transport,
                             PacketTransport::Connection* closedConnection) {
    if (closedConnection) {
        *closedConnection = 0;
    }
    if (!m_open || m_failure != Failure::None) {
        return false;
    }
    const auto time = now();
    if (!time) {
        return false;
    }
    for (u8 destination = 1; destination <= 4; ++destination) {
        if (m_links[destination] == 0) {
            continue;
        }
        bool congested = false;
        const u8 firstOrigin = m_sendOrigin[destination];
        for (u8 offset = 0; offset < 4 && !congested; ++offset) {
            const u8 origin = static_cast<u8>((firstOrigin - 1 + offset) % 4 + 1);
            if (!occupied(origin) || origin == destination || (m_local != 1 && origin != m_local) ||
                m_progress[origin].serial == 0) {
                continue;
            }
            const auto& history = m_history[origin];
            std::set<u64> selected;
            // Resend from this receiver's own ACK, never the relay's ACK.
            auto first = history.upper_bound(m_received[destination][origin]);
            for (usize count = 0; count < RollbackInputs::kRedundantTicks && first != history.end();
                 ++count, ++first) {
                selected.insert(first->first);
            }
            usize count = 0;
            for (auto frame = history.rbegin();
                 count < RollbackInputs::kRedundantTicks && frame != history.rend();
                 ++count, ++frame) {
                selected.insert(frame->first);
            }
            const auto& progress = m_progress[origin];
            Message message{m_epoch,
                            origin,
                            m_next[origin],
                            m_received[origin],
                            {},
                            progress.serial,
                            addAge(progress.ageUs, *time - progress.observedAt)};
            usize frames = 0;
            usize inputBytes = InputPacket::kHeaderBytes;
            const auto send = [&] {
                const auto encoded = encode(message);
                if (!encoded) {
                    return fail(Failure::Transport);
                }
                const auto result = transport.send(m_links[destination], *encoded,
                                                   PacketTransport::Delivery::Unreliable);
                if (result == PacketTransport::SendResult::Congested) {
                    congested = true;
                    return true;
                }
                if (result == PacketTransport::SendResult::Sent) {
                    // A one-datagram send budget must not always go to origin 1.
                    m_sendOrigin[destination] = static_cast<u8>(origin % 4 + 1);
                    return true;
                }
                if (m_local != 1 && destination != 1) {
                    // A direct link that will not carry is let go; the host relays.
                    m_links[destination] = 0;
                    congested = true;
                    return true;
                }
                if (result == PacketTransport::SendResult::Disconnected && closedConnection) {
                    // A provider can observe closure in send before its asynchronous
                    // Disconnected event is polled. Let the session apply its normal
                    // departure policy without first poisoning the input stream.
                    *closedConnection = m_links[destination];
                    return false;
                }
                return fail(Failure::Transport);
            };
            for (const u64 tick : selected) {
                const auto& frame = history.at(tick);
                usize frameBytes = 0;
                for (const auto& command : frame) {
                    frameBytes += InputPacket::commandBytes(command);
                }
                if (InputPacket::kHeaderBytes + frameBytes > kMaxBytes - kHeaderBytes) {
                    return fail(Failure::Transport);
                }
                if (message.commands.size() + frame.size() > InputPacket::kMaxCommands ||
                    frames == RollbackInputs::kRedundantTicks ||
                    inputBytes + frameBytes > kMaxBytes - kHeaderBytes) {
                    if (!send()) {
                        return false;
                    }
                    if (congested) {
                        break;
                    }
                    message.commands.clear();
                    frames = 0;
                    inputBytes = InputPacket::kHeaderBytes;
                }
                message.commands.insert(message.commands.end(), frame.begin(), frame.end());
                inputBytes += frameBytes;
                ++frames;
            }
            // Empty heartbeats carry ACK/progress even before the first sample.
            if (!congested && !send()) {
                return false;
            }
        }
    }
    return true;
}
bool RollbackExchange::fail(Failure reason) {
    if (m_failure == Failure::None) {
        m_failure = reason;
    }
    return false;
}
void RollbackExchange::suspend(u8 peer) {
    if (peer == 0 || peer > 4 || !occupied(peer)) {
        return;
    }
    m_suspended[peer] = true;
    m_links[peer] = 0;
}
void RollbackExchange::detach(PacketTransport::Connection connection) {
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (connection != 0 && m_links[peer] == connection) {
            m_links[peer] = 0;
        }
    }
}
void RollbackExchange::disconnected(PacketTransport::Connection connection) {
    if (connection != 0 && std::ranges::find(m_links, connection) != m_links.end()) {
        fail(Failure::Disconnected);
    }
}
u64 RollbackExchange::receivedThrough(u8 origin) const {
    return occupied(origin) ? m_received[m_local][origin] : 0;
}
u64 RollbackExchange::acknowledgedThrough(u8 receiver, u8 origin) const {
    return occupied(receiver) && occupied(origin) ? m_received[receiver][origin] : 0;
}
usize RollbackExchange::retainedFrames(u8 origin) const {
    return occupied(origin) ? m_history[origin].size() : 0;
}
std::optional<u64> RollbackExchange::hostReceipt(u8 origin) const {
    return occupied(origin) && m_direct[origin] ? std::optional{m_received[1][origin]}
                                                : std::nullopt;
}
} // namespace gdl::game
