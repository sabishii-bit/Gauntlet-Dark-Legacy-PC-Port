#include "game/netplay/NativeAgreement.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDNA");
constexpr u32 kTerminalMagic = fourcc("GDNT");
enum class Kind : u8 { Report = 1, Accepted, Failed };
using Failure = NativeAgreement::Failure;
using Digest = NativeAgreement::Digest;
using Bytes = std::array<u8, NativeAgreement::kPacketBytes>;
/** A packet's magic, version, kind, failure and zero byte, before its epoch, tick and digest. */
constexpr usize kHeaderBytes = 8;
static_assert(NativeAgreement::kPacketBytes ==
              kHeaderBytes + 2 * sizeof(u64) + std::tuple_size_v<Digest>);

std::string digestText(const Digest& digest) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const u8 byte : digest) {
        result.push_back(kDigits[byte >> 4U]);
        result.push_back(kDigits[byte & 15U]);
    }
    return result;
}

Bytes encode(Kind kind, u64 epoch, u64 tick, const Digest& digest, Failure failure = Failure::None,
             bool terminal = false) {
    std::vector<u8> written;
    written.reserve(NativeAgreement::kPacketBytes);
    ByteWriter out(written);
    out.writeU32(terminal ? kTerminalMagic : kMagic);
    out.writeU8(NativeAgreement::kVersion);
    out.writeU8(static_cast<u8>(kind));
    out.writeU8(static_cast<u8>(failure));
    out.writeU8(0);
    out.writeU64(epoch);
    out.writeU64(tick);
    out.writeBytes(digest);
    Bytes bytes{};
    std::ranges::copy(written, bytes.begin());
    return bytes;
}
struct Message {
    Kind kind;
    Failure failure;
    u64 epoch;
    u64 tick;
    Digest digest;
    bool terminal;
};
std::optional<Message> decode(std::span<const u8> bytes) {
    if (bytes.size() != NativeAgreement::kPacketBytes || !NativeAgreement::recognizes(bytes) ||
        bytes[4] != NativeAgreement::kVersion || bytes[7] != 0 ||
        bytes[5] < static_cast<u8>(Kind::Report) || bytes[5] > static_cast<u8>(Kind::Failed) ||
        bytes[6] > static_cast<u8>(Failure::Transport)) {
        return std::nullopt;
    }
    Message result{static_cast<Kind>(bytes[5]),
                   static_cast<Failure>(bytes[6]),
                   0,
                   0,
                   {},
                   NativeAgreement::recognizesTerminal(bytes)};
    ByteReader reader(bytes);
    reader.skip(kHeaderBytes);
    result.epoch = reader.readU64();
    result.tick = reader.readU64();
    std::ranges::copy(reader.readBytes(result.digest.size()), result.digest.begin());
    if (result.epoch == 0 || (result.kind == Kind::Failed) != (result.failure != Failure::None) ||
        (result.kind == Kind::Failed && result.digest != Digest{})) {
        return std::nullopt;
    }
    return result;
}
} // namespace

bool NativeAgreement::expired(Time start, Time now) const {
    const auto limit = timeout();
    // Avoid subtracting extreme injected time points or overflowing start+timeout.
    return start <= Time::max() - limit && now >= start + limit;
}

bool NativeAgreement::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 &&
           (ByteReader(bytes).readU32() == kMagic || recognizesTerminal(bytes));
}
bool NativeAgreement::recognizesTerminal(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kTerminalMagic;
}
std::optional<std::pair<u64, u64>> NativeAgreement::terminalContext(std::span<const u8> bytes) {
    const auto message = decode(bytes);
    if (!message || !message->terminal || (message->tick == 0 && message->kind != Kind::Failed)) {
        return std::nullopt;
    }
    return std::pair{message->epoch, message->tick};
}
bool NativeAgreement::occupied(u8 peer) const {
    return peer != 0 && !m_suspended[peer] && std::ranges::find(m_owners, peer) != m_owners.end();
}
void NativeAgreement::suspend(u8 peer, Time now) {
    if (!m_open || peer == 0 || peer > 4 || peer == m_local || !occupied(peer)) {
        return;
    }
    m_suspended[peer] = true;
    m_outgoing[peer].clear();
    m_links[peer] = 0;
    // A verdict the drop settles is sent from now, not from the stream's last call.
    m_now = std::max(m_now, now);
    if (!failed()) {
        settle();
    }
}
bool NativeAgreement::open(u8 localPeer, u64 epoch, const Owners& owners,
                           std::span<const Link> links, u32 period, Time now, u64 agreedThrough) {
    if (m_open || localPeer == 0 || localPeer > 4 || epoch == 0 || period == 0 ||
        period > kMaxPeriod || agreedThrough % period != 0 ||
        std::ranges::find(owners, localPeer) == owners.end() ||
        std::ranges::find(owners, 1) == owners.end() ||
        std::ranges::any_of(owners, [](u8 peer) { return peer > 4; })) {
        return false;
    }
    std::array<PacketTransport::Connection, 5> connections{};
    std::set<PacketTransport::Connection> unique;
    for (const auto& link : links) {
        if (link.peer == 0 || link.peer > 4 || link.peer == localPeer || link.connection == 0 ||
            connections[link.peer] != 0 || !unique.insert(link.connection).second ||
            std::ranges::find(owners, link.peer) == owners.end() ||
            (localPeer != 1 && link.peer != 1)) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        const bool needed = peer != localPeer && std::ranges::find(owners, peer) != owners.end() &&
                            (localPeer == 1 || peer == 1);
        if ((connections[peer] != 0) != needed) {
            return false;
        }
    }
    m_open = true;
    m_local = localPeer;
    m_epoch = epoch;
    m_period = period;
    m_agreed = agreedThrough;
    m_submit = agreedThrough + period;
    m_now = now;
    m_owners = owners;
    m_links = connections;
    return true;
}
bool NativeAgreement::openTerminal(u8 localPeer, u64 epoch, const Owners& owners,
                                   std::span<const Link> links, u64 terminalTick, Time now) {
    if (terminalTick == 0 || !open(localPeer, epoch, owners, links, 1, now)) {
        return false;
    }
    m_terminal = terminalTick;
    m_submit = terminalTick;
    return true;
}
bool NativeAgreement::scheduled(u64 tick) const {
    return m_terminal ? tick == *m_terminal : tick != 0 && m_period != 0 && tick % m_period == 0;
}
bool NativeAgreement::inWindow(u64 tick) const {
    return tick > m_agreed &&
           (m_terminal ? tick == *m_terminal : (tick - m_agreed) / m_period <= kWindow);
}
bool NativeAgreement::queueReady() const {
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && m_outgoing[peer].size() >= kQueuedPackets) {
            return false;
        }
    }
    return true;
}
bool NativeAgreement::canSubmit() const {
    return m_open && !failed() && m_submit != 0 && inWindow(m_submit) && queueReady();
}
usize NativeAgreement::queuedPackets() const {
    usize count = 0;
    for (const auto& queue : m_outgoing) {
        count += queue.size();
    }
    return count;
}
void NativeAgreement::fail(Failure reason, u64 tick) {
    if (failed()) {
        return;
    }
    m_failure = reason;
    m_failureTick = tick;
    m_pending.clear();
    const auto bytes = encode(Kind::Failed, m_epoch, tick, {}, reason, m_terminal.has_value());
    for (u8 peer = 1; peer <= 4; ++peer) {
        // Terminal notification has reserved bounded capacity even when ordinary
        // reports/verdicts are congested. Already sent packets remain ordered.
        m_outgoing[peer].clear();
        if (m_links[peer] != 0) {
            m_outgoing[peer].push_back({bytes, m_now});
        }
    }
}
bool NativeAgreement::update(Time now) {
    if (!m_open || failed()) {
        return false;
    }
    m_now = std::max(m_now, now);
    for (const auto& [tick, pending] : m_pending) {
        if (expired(pending.started, m_now) && (m_local != 1 || !pending.digests[1])) {
            fail(Failure::Timeout, tick);
            return false;
        }
    }
    if (m_local == 1) {
        return true; // a verdict a guest has not taken in time makes that guest overdue
    }
    // An array iterator is a pointer only on some libraries:
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto overdue = std::ranges::find_if(m_outgoing, [&](const auto& queue) {
        return !queue.empty() && expired(queue.front().started, m_now);
    });
    if (overdue == m_outgoing.end()) {
        return true;
    }
    const auto message = decode(overdue->front().bytes);
    fail(Failure::Timeout, message->tick);
    return false;
}
std::vector<u8> NativeAgreement::overdue(Time now) const {
    std::vector<u8> late;
    if (!m_open || failed() || m_local != 1) {
        return late;
    }
    const Time at = std::max(m_now, now);
    const auto add = [&late](u8 peer) {
        if (std::ranges::find(late, peer) == late.end()) {
            late.push_back(peer);
        }
    };
    for (const auto& entry : m_pending) {
        const Pending& pending = entry.second;
        if (!expired(pending.started, at)) {
            continue;
        }
        if (!pending.digests[1]) {
            return {}; // the host's own digest is what is late: nobody else to blame
        }
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (occupied(peer) && !pending.digests[peer]) {
                add(peer);
            }
        }
    }
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (!m_outgoing[peer].empty() && expired(m_outgoing[peer].front().started, at)) {
            add(peer);
        }
    }
    return late;
}
void NativeAgreement::rearm(Time now) {
    if (!m_open || failed()) {
        return;
    }
    m_now = std::max(m_now, now);
    for (auto& entry : m_pending) {
        entry.second.started = m_now;
    }
    for (auto& queue : m_outgoing) {
        for (auto& delivery : queue) {
            delivery.started = m_now;
        }
    }
}
NativeAgreement::Admission NativeAgreement::admit(u8 peer, u64 tick, const Digest& digest) {
    if (tick <= m_agreed) {
        const auto previous = m_history.find(tick);
        if (previous == m_history.end()) {
            return Admission::Stale;
        }
        if (previous->second != digest) {
            log::error("Native digest conflict: epoch={} tick={} local={} origin={} "
                       "confirmed=true previous={} received={}",
                       m_epoch, tick, m_local, peer, digestText(previous->second),
                       digestText(digest));
            fail(Failure::Conflict, tick);
            return Admission::Failed;
        }
        return Admission::Duplicate;
    }
    if (!inWindow(tick)) {
        return Admission::Full;
    }
    const auto entry = m_pending.try_emplace(tick, Pending{{}, false, m_now}).first;
    auto& previous = entry->second.digests[peer];
    if (previous) {
        if (*previous != digest) {
            log::error("Native digest conflict: epoch={} tick={} local={} origin={} "
                       "confirmed=false previous={} received={}",
                       m_epoch, tick, m_local, peer, digestText(*previous), digestText(digest));
            fail(Failure::Conflict, tick);
            return Admission::Failed;
        }
        return Admission::Duplicate;
    }
    previous = digest;
    return Admission::Accepted;
}
void NativeAgreement::settle() {
    while (!m_pending.empty() && !failed()) {
        const auto entry = m_pending.begin();
        const u64 tick = entry->first;
        const auto& pending = entry->second;
        if ((m_terminal ? tick != *m_terminal || m_agreed != 0 : tick - m_agreed != m_period) ||
            !pending.digests[m_local]) {
            return;
        }
        const Digest digest = *pending.digests[m_local];
        if (m_local == 1) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (occupied(peer) && !pending.digests[peer]) {
                    return;
                }
            }
            bool mismatch = false;
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (occupied(peer) && *pending.digests[peer] != digest) {
                    log::warn("Native digest mismatch: epoch={} tick={} host={} peer={} guest={} "
                              "agreed={} pending={} terminal={}",
                              m_epoch, tick, digestText(digest), peer,
                              digestText(*pending.digests[peer]), m_agreed, m_pending.size(),
                              m_terminal.has_value());
                    m_mismatched[peer] = true;
                    mismatch = true;
                }
            }
            if (mismatch) {
                fail(Failure::Mismatch, tick);
                return;
            }
            if (!queueReady()) {
                return;
            }
            const auto bytes = encode(Kind::Accepted, m_epoch, tick, digest, Failure::None,
                                      m_terminal.has_value());
            for (u8 peer = 2; peer <= 4; ++peer) {
                if (m_links[peer] != 0) {
                    m_outgoing[peer].push_back({bytes, m_now});
                }
            }
        } else if (!pending.accepted) {
            return;
        }
        m_history.emplace(tick, digest);
        if (m_history.size() > kHistory) {
            m_history.erase(m_history.begin());
        }
        m_agreed = tick;
        m_pending.erase(entry);
    }
}
NativeAgreement::Admission NativeAgreement::submit(u64 tick, const Digest& digest, Time now) {
    if (!m_open || !scheduled(tick)) {
        return Admission::Invalid;
    }
    if (!update(now)) {
        return Admission::Failed;
    }
    if (tick < m_submit || m_submit == 0) {
        const auto previous = m_pending.find(tick);
        if (tick <= m_agreed ||
            (previous != m_pending.end() && previous->second.digests[m_local])) {
            return admit(m_local, tick, digest);
        }
        return Admission::Invalid;
    }
    if (tick != m_submit) {
        return Admission::Invalid;
    }
    if (!canSubmit()) {
        return Admission::Full;
    }
    const auto result = admit(m_local, tick, digest);
    if (result != Admission::Accepted) {
        return result;
    }
    if (m_local != 1) {
        m_outgoing[1].push_back(
            {encode(Kind::Report, m_epoch, tick, digest, Failure::None, m_terminal.has_value()),
             m_now});
    }
    m_submit =
        m_terminal || tick > std::numeric_limits<u64>::max() - m_period ? 0 : tick + m_period;
    settle();
    return failed() ? Admission::Failed : Admission::Accepted;
}
NativeAgreement::Admission NativeAgreement::receive(PacketTransport::Connection connection,
                                                    std::span<const u8> bytes, Time now) {
    if (!m_open) {
        return Admission::Invalid;
    }
    // An array iterator is a pointer only on some libraries:
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto link = std::ranges::find(m_links, connection);
    if (connection == 0 || link == m_links.end()) {
        return Admission::WrongPeer;
    }
    const auto message = decode(bytes);
    if (!message) {
        fail(Failure::Protocol, 0);
        return Admission::Invalid;
    }
    if (message->epoch != m_epoch || message->terminal != m_terminal.has_value()) {
        return Admission::Stale;
    }
    if ((!scheduled(message->tick) && (message->kind != Kind::Failed || message->tick != 0)) ||
        (m_local == 1 && message->kind == Kind::Accepted) ||
        (m_local != 1 && message->kind == Kind::Report)) {
        fail(Failure::Protocol, 0);
        return Admission::Invalid;
    }
    if (failed()) {
        return message->kind == Kind::Failed && message->failure == m_failure &&
                       message->tick == m_failureTick
                   ? Admission::Duplicate
                   : Admission::Failed;
    }
    if (!update(now)) {
        return Admission::Failed;
    }
    if (message->kind == Kind::Failed) {
        fail(message->failure, message->tick);
        return Admission::Failed;
    }
    if (message->kind == Kind::Report) {
        const auto peer = static_cast<u8>(link - m_links.begin());
        const auto result = admit(peer, message->tick, message->digest);
        if (result == Admission::Full) {
            fail(Failure::Window, message->tick);
            return Admission::Full;
        }
        settle();
        return failed() ? Admission::Failed : result;
    }
    if (message->tick <= m_agreed) {
        return admit(m_local, message->tick, message->digest);
    }
    const auto entry = m_pending.find(message->tick);
    if (entry == m_pending.end() || !entry->second.digests[m_local]) {
        fail(Failure::Protocol, message->tick);
        return Admission::Invalid;
    }
    if (*entry->second.digests[m_local] != message->digest) {
        log::error("Native host verdict mismatch: epoch={} tick={} peer={} local={} host={}",
                   m_epoch, message->tick, m_local, digestText(*entry->second.digests[m_local]),
                   digestText(message->digest));
        fail(Failure::Mismatch, message->tick);
        return Admission::Failed;
    }
    const bool duplicate = entry->second.accepted;
    entry->second.accepted = true;
    settle();
    return duplicate ? Admission::Duplicate : Admission::Accepted;
}
bool NativeAgreement::flush(PacketTransport& transport, Time now,
                            PacketTransport::Connection* closedConnection) {
    if (closedConnection != nullptr) {
        *closedConnection = 0;
    }
    if (!m_open) {
        return false;
    }
    update(now);
    for (u8 peer = 1; peer <= 4; ++peer) {
        auto& queue = m_outgoing[peer];
        while (m_links[peer] != 0 && !queue.empty()) {
            const auto result = transport.send(m_links[peer], queue.front().bytes,
                                               PacketTransport::Delivery::Reliable);
            if (result == PacketTransport::SendResult::Congested) {
                break;
            }
            if (result == PacketTransport::SendResult::Disconnected &&
                closedConnection != nullptr && !failed()) {
                *closedConnection = m_links[peer];
                return false;
            }
            if (result != PacketTransport::SendResult::Sent) {
                m_links[peer] = 0;
                queue.clear();
                fail(Failure::Transport, m_pending.empty() ? 0 : m_pending.begin()->first);
                break;
            }
            queue.pop_front();
        }
    }
    settle();
    return !failed();
}
void NativeAgreement::disconnected(PacketTransport::Connection connection) {
    if (!m_open || connection == 0) {
        return;
    }
    // An array iterator is a pointer only on some libraries:
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto link = std::ranges::find(m_links, connection);
    if (link != m_links.end()) {
        m_outgoing[static_cast<usize>(link - m_links.begin())].clear();
        *link = 0;
        fail(Failure::Disconnected, m_pending.empty() ? 0 : m_pending.begin()->first);
    }
}
} // namespace gdl::game
