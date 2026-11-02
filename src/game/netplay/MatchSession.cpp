#include "game/netplay/MatchSession.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLT");
constexpr u32 kConsoleMagic = fourcc("GDLC");
constexpr u8 kConsoleVersion = 1;
constexpr usize kConsoleHeader = 37;
std::vector<u8> consolePacket(bool permission, bool enabled, u64 epoch, u64 revision, u64 request,
                              u32 grant, const ConsoleAction* action = nullptr) {
    std::vector<u8> bytes;
    ByteWriter out(bytes);
    out.writeU32(kConsoleMagic);
    out.writeU8(kConsoleVersion);
    out.writeU8(static_cast<u8>(permission ? 0 : 1));
    out.writeU8(static_cast<u8>(enabled));
    out.writeU8(action ? action->seat : u8{0});
    out.writeU64(epoch);
    out.writeU64(revision);
    out.writeU64(request);
    out.writeU32(grant);
    out.writeU8(static_cast<u8>(action ? action->line.size() : 0));
    if (action) {
        out.writeString(action->line);
    }
    return bytes;
}
bool validOwners(const MatchOwners& owners) {
    return std::ranges::all_of(owners, [](u8 peer) { return peer <= 4; }) &&
           std::ranges::find(owners, 1) != owners.end();
}
bool belongs(const MatchOwners& owners, u8 peer) {
    return peer != 0 && std::ranges::find(owners, peer) != owners.end();
}
/** A link a drop took down: whatever still arrives on it is nothing new. */
bool droppedLink(std::span<const PacketTransport::Connection> dropped,
                 PacketTransport::Connection connection) {
    return connection != 0 && std::ranges::find(dropped, connection) != dropped.end();
}
u8 seatsOf(const MatchOwners& owners, u8 peer = 0) {
    u8 mask = 0;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] != 0 && (peer == 0 || owners[seat] == peer)) {
            mask |= static_cast<u8>(1U << seat);
        }
    }
    return mask;
}
/** Every dropped peer's seats: they count as having voted to skip an entry movie. */
u8 droppedSeats(const MatchOwners& owners, std::span<const MatchSession::Drop> dropped) {
    u8 seats = 0;
    for (const auto& drop : dropped) {
        if (drop.peer != 0) {
            seats |= seatsOf(owners, drop.peer);
        }
    }
    return seats;
}
bool checkpointed(MatchTransition transition) {
    return transition == MatchTransition::Travel || transition == MatchTransition::Shop ||
           transition == MatchTransition::SecretEnter ||
           transition == MatchTransition::SecretReturn;
}
bool hasEntryMovie(MatchTransition transition) {
    return transition == MatchTransition::Start || transition == MatchTransition::Travel;
}
std::string_view agreementReason(NativeAgreement::Failure reason) {
    switch (reason) {
    case NativeAgreement::Failure::None: return "none";
    case NativeAgreement::Failure::Mismatch: return "mismatch";
    case NativeAgreement::Failure::Conflict: return "conflicting digest";
    case NativeAgreement::Failure::Timeout: return "timeout";
    case NativeAgreement::Failure::Window: return "bounded window exhausted";
    case NativeAgreement::Failure::Protocol: return "invalid agreement protocol";
    case NativeAgreement::Failure::Disconnected: return "disconnected";
    case NativeAgreement::Failure::Transport: return "transport failure";
    }
    return "unknown";
}
std::string_view inputFailureReason(RollbackExchange::Failure reason) {
    switch (reason) {
    case RollbackExchange::Failure::None: return "invalid input or progress";
    case RollbackExchange::Failure::Conflict: return "conflicting input history";
    case RollbackExchange::Failure::Window: return "input history window exhausted";
    case RollbackExchange::Failure::Disconnected: return "input link disconnected";
    case RollbackExchange::Failure::Transport: return "input transport or clock failure";
    }
    return "unknown input failure";
}
std::string_view inputAdmissionReason(RollbackExchange::Admission result) {
    switch (result) {
    case RollbackExchange::Admission::Accepted: return "accepted";
    case RollbackExchange::Admission::WrongPeer: return "wrong peer";
    case RollbackExchange::Admission::WrongEpoch: return "wrong epoch";
    case RollbackExchange::Admission::Invalid: return "invalid input packet";
    case RollbackExchange::Admission::Conflict: return "conflicting input history";
    case RollbackExchange::Admission::Full: return "input history window exhausted";
    case RollbackExchange::Admission::Rejected: return "simulation rejected input or progress";
    }
    return "unknown input admission";
}
} // namespace

bool MatchContext::valid() const {
    if (epoch == 0 || scene == 0 || scene > 65535 || tickRate != 60 ||
        inputLead > RollbackInputs::kMaxDelay || transition == MatchTransition::Resume ||
        static_cast<u8>(transition) > 5 || !validOwners(owners) ||
        (challengeWon && transition != MatchTransition::SecretReturn)) {
        return false;
    }
    const bool fenced = checkpointed(transition);
    if (fenced ? (sourceEpoch == 0 || sourceEpoch != epoch - 1)
               : (sourceEpoch != 0 || sourceTick != 0)) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if ((owners[seat] != 0) != (grants[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool MatchControl::valid() const {
    const bool vote = kind == MatchControlKind::MovieSkipVote;
    const bool resync = kind == MatchControlKind::Resync;
    const bool done = kind == MatchControlKind::ResyncDone;
    const bool drop = kind == MatchControlKind::Drop;
    constexpr u8 kGuestBits = 0b11100; // peers two to four
    if ((resyncTick != 0) != (resync || drop) || (resyncPeers != 0) != (resync || done || drop) ||
        (resync && resyncTick % MatchSession::kNativeAgreementPeriod != 0) ||
        (resyncPeers & ~kGuestBits) != 0 || ((done || drop) && std::popcount(resyncPeers) != 1)) {
        return false;
    }
    return context.valid() && static_cast<u8>(kind) <= 9 && static_cast<u8>(stop) <= 4 &&
           ((kind == MatchControlKind::Stop) == (stop != MatchStop::None)) &&
           (vote == (movieSkipVotes != 0)) && (movieSkipVotes & ~seatsOf(context.owners)) == 0 &&
           (!vote || hasEntryMovie(context.transition)) && kind != MatchControlKind::Pause &&
           kind != MatchControlKind::RequestPause;
}
std::optional<std::vector<u8>> MatchControlPacket::encode(const MatchControl& control) {
    if (!control.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    ByteWriter out(bytes);
    out.writeU32(kMagic);
    out.writeU8(kVersion);
    out.writeU8(static_cast<u8>(control.kind));
    out.writeU8(static_cast<u8>(control.stop));
    out.writeU8(control.movieSkipVotes);
    const auto& context = control.context;
    out.writeU64(context.epoch);
    out.writeU32(context.scene);
    out.writeU16(context.tickRate);
    out.writeU8(context.inputLead);
    out.writeU8(static_cast<u8>(context.transition));
    out.writeBytes(context.owners);
    for (const auto grant : context.grants) {
        out.writeU32(grant);
    }
    out.writeU32(static_cast<u32>(context.challengeWon) | (u32{control.resyncPeers} << 8U));
    out.writeU64(context.sourceEpoch);
    out.writeU64(context.sourceTick);
    out.writeU64(control.resyncTick);
    return bytes;
}
std::optional<MatchControl> MatchControlPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU8() != kVersion) {
        return std::nullopt;
    }
    MatchControl control;
    control.kind = static_cast<MatchControlKind>(reader.readU8());
    control.stop = static_cast<MatchStop>(reader.readU8());
    control.movieSkipVotes = reader.readU8();
    auto& context = control.context;
    context.epoch = reader.readU64();
    context.scene = reader.readU32();
    context.tickRate = reader.readU16();
    context.inputLead = reader.readU8();
    context.transition = static_cast<MatchTransition>(reader.readU8());
    for (auto& owner : context.owners) {
        owner = reader.readU8();
    }
    for (auto& grant : context.grants) {
        grant = reader.readU32();
    }
    const auto flags = reader.readU32();
    const auto challengeWon = flags & 0xFFU;
    if (challengeWon > 1 || (flags >> 16U) != 0) {
        return std::nullopt;
    }
    context.challengeWon = challengeWon != 0;
    control.resyncPeers = static_cast<u8>(flags >> 8U);
    context.sourceEpoch = reader.readU64();
    context.sourceTick = reader.readU64();
    control.resyncTick = reader.readU64();
    return control.valid() ? std::optional{control} : std::nullopt;
}

bool MatchSession::open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links,
                        u8 startupDelay, std::span<const MatchLink> direct) {
    if (m_phase != Phase::Offline || !validOwners(owners) || !belongs(owners, localPeer) ||
        startupDelay > RollbackInputs::kMaxDelay) {
        return false;
    }
    std::array<PacketTransport::Connection, 5> connections{};
    for (const auto& link : links) {
        if (!belongs(owners, link.peer) || link.peer == localPeer || link.connection == 0 ||
            (localPeer != 1 && link.peer != 1) || connections[link.peer] != 0 ||
            std::ranges::find(connections, link.connection) != connections.end()) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (peer != localPeer && belongs(owners, peer) && (localPeer == 1 || peer == 1) &&
            connections[peer] == 0) {
            return false;
        }
    }
    std::array<PacketTransport::Connection, 5> directConnections{};
    for (const auto& link : direct) {
        if (localPeer == 1 || !belongs(owners, link.peer) || link.peer == localPeer ||
            link.peer == 1 || link.connection == 0 || directConnections[link.peer] != 0 ||
            std::ranges::find(connections, link.connection) != connections.end() ||
            std::ranges::find(directConnections, link.connection) != directConnections.end()) {
            return false;
        }
        directConnections[link.peer] = link.connection;
    }
    m_local = localPeer;
    m_owners = owners;
    m_links = connections;
    m_direct = directConnections;
    m_startupDelay = startupDelay;
    m_phase = Phase::Lobby;
    return true;
}
void MatchSession::clear() {
    *this = MatchSession{};
}
bool MatchSession::consoleReady() const {
    return m_phase == Phase::Running && m_context.transition != MatchTransition::Shop &&
           !m_resync && !m_pendingNativeContext && !m_nativeTerminal;
}
bool MatchSession::setConsolePermissions(bool enabled) {
    if (!host() || m_phase == Phase::Offline || m_phase == Phase::Stopped ||
        m_consoleRevision == std::numeric_limits<u64>::max()) {
        return false;
    }
    if (enabled == m_consolePermissions) {
        return true;
    }
    const auto peers = std::ranges::count_if(m_links, [](auto link) { return link != 0; });
    if (m_controls.size() + static_cast<usize>(peers) > kControlBudget) {
        return false;
    }
    ++m_consoleRevision;
    m_consolePermissions = enabled;
    const auto bytes = consolePacket(true, enabled, 0, m_consoleRevision, 0, 0);
    for (const auto link : m_links) {
        if (link != 0) {
            m_controls.push_back({link, bytes});
        }
    }
    if (!enabled) {
        std::erase_if(m_consoleRequests, [](const auto& request) { return request.peer != 1; });
    }
    return true;
}
bool MatchSession::admitConsole(u8 peer, u64 epoch, u64 revision, u64 request, u32 grant,
                                const ConsoleAction& action) {
    if (!host() || !consoleReady() || !action.valid() || peer == 0 || peer > 4 || dropped(peer) ||
        epoch != m_context.epoch || m_context.owners[action.seat] != peer ||
        m_context.grants[action.seat] != grant || request == 0 || request <= m_consoleSeen[peer] ||
        (peer != 1 && (!m_consolePermissions || revision != m_consoleRevision)) ||
        m_consoleRequests.size() >= kConsoleRequests) {
        return false;
    }
    m_consoleSeen[peer] = request;
    m_consoleRequests.push_back({peer, epoch, action});
    return true;
}
bool MatchSession::requestConsole(const ConsoleAction& action) {
    if (!consoleReady() || !action.valid() || m_context.owners[action.seat] != m_local ||
        (!host() && !m_consolePermissions) ||
        m_consoleNextRequest == std::numeric_limits<u64>::max()) {
        return false;
    }
    const u64 request = m_consoleNextRequest + 1;
    const u32 grant = m_context.grants[action.seat];
    if (host()) {
        if (!admitConsole(m_local, m_context.epoch, m_consoleRevision, request, grant, action)) {
            return false;
        }
    } else {
        if (m_links[1] == 0 || m_controls.size() >= kControlBudget) {
            return false;
        }
        m_controls.push_back(
            {m_links[1], consolePacket(false, false, m_context.epoch, m_consoleRevision, request,
                                       grant, &action)});
    }
    m_consoleNextRequest = request;
    return true;
}
std::optional<ConsoleAction> MatchSession::takeConsoleRequest() {
    if (!host() || !consoleReady()) {
        m_consoleRequests.clear();
        return std::nullopt;
    }
    while (!m_consoleRequests.empty()) {
        auto request = std::move(m_consoleRequests.front());
        m_consoleRequests.pop_front();
        if (request.epoch == m_context.epoch && !dropped(request.peer) &&
            (request.peer == 1 || m_consolePermissions)) {
            return std::move(request.action);
        }
    }
    return std::nullopt;
}
MatchSession::Admission MatchSession::consoleMessage(u8 peer, std::span<const u8> bytes) {
    if (bytes.size() < kConsoleHeader || bytes.size() > kConsoleHeader + ConsoleAction::kMaxLine) {
        return Admission::Invalid;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kConsoleMagic || reader.readU8() != kConsoleVersion) {
        return Admission::Invalid;
    }
    const u8 kind = reader.readU8();
    const u8 enabled = reader.readU8();
    ConsoleAction action;
    action.seat = reader.readU8();
    const u64 epoch = reader.readU64();
    const u64 revision = reader.readU64();
    const u64 request = reader.readU64();
    const u32 grant = reader.readU32();
    const usize length = reader.readU8();
    if (kind > 1 || enabled > 1 || length != reader.remaining()) {
        return Admission::Invalid;
    }
    if (kind == 0) {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (epoch != 0 || request != 0 || grant != 0 || action.seat != 0 || length != 0 ||
            revision == 0) {
            return Admission::Invalid;
        }
        if (revision <= m_consoleRevision) {
            return revision == m_consoleRevision && (enabled != 0) != m_consolePermissions
                       ? Admission::Invalid
                       : Admission::Stale;
        }
        m_consoleRevision = revision;
        m_consolePermissions = enabled != 0;
        return Admission::Accepted;
    }
    if (!host() || peer == 1) {
        return Admission::WrongPeer;
    }
    action.line = reader.readString(length);
    if (enabled != 0 || !action.valid() || request == 0) {
        return Admission::Invalid;
    }
    if (epoch != m_context.epoch || !consoleReady() || request <= m_consoleSeen[peer] ||
        revision != m_consoleRevision || !m_consolePermissions) {
        return Admission::Stale;
    }
    if (m_context.owners[action.seat] != peer || m_context.grants[action.seat] != grant) {
        return Admission::WrongPeer;
    }
    return admitConsole(peer, epoch, revision, request, grant, action) ? Admission::Accepted
                                                                       : Admission::Stale;
}
u8 MatchSession::directPeerOf(PacketTransport::Connection connection) const {
    if (connection != 0) {
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (m_direct[peer] == connection) {
                return peer;
            }
        }
    }
    return 0;
}
usize MatchSession::directLinks() const {
    return static_cast<usize>(std::ranges::count_if(m_direct, [](auto link) { return link != 0; }));
}
void MatchSession::directLost(PacketTransport::Connection connection) {
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (connection != 0 && m_direct[peer] == connection) {
            m_direct[peer] = 0;
        }
    }
    for (auto* exchange : {m_exchange.get(), m_drainingInputs.get()}) {
        if (exchange) {
            exchange->detach(connection);
        }
    }
}
u8 MatchSession::peerOf(PacketTransport::Connection connection) const {
    if (connection != 0) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (m_links[peer] == connection) {
                return peer;
            }
        }
    }
    return 0;
}
bool MatchSession::queue(u8 peer, MatchControlKind kind, MatchStop reason, u8 votes,
                         Resync resync) {
    const auto bytes =
        MatchControlPacket::encode({kind, reason, m_context, votes, resync.tick, resync.peers});
    if (peer == 0 || peer > 4 || m_links[peer] == 0 || !bytes ||
        m_controls.size() >= kControlBudget) {
        return false;
    }
    m_controls.push_back({m_links[peer], *bytes});
    return true;
}
bool MatchSession::broadcast(MatchControlKind kind, MatchStop reason, u8 votes, Resync resync) {
    const auto peers =
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; });
    if (m_controls.size() + static_cast<usize>(peers) > kControlBudget) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && !queue(peer, kind, reason, votes, resync)) {
            return false;
        }
    }
    return true;
}
void MatchSession::discardGameplay() {
    m_consoleRequests.clear();
    m_resync.reset();
    m_rebuiltFrom = {};
    m_repairs = 0;
    m_journalQueuedPeers = 0;
    m_resyncTarget = false;
    m_journalComplete = false;
    m_journalIncoming.clear();
    m_journalDeliveries.clear();
    m_recentDigests.clear();
    m_exchange.reset();
    m_nativeConsumer = {};
    m_nativeAgreement.reset();
    m_terminalAgreement.reset();
    m_nativeNextTick = 0;
    m_nativeCompleted = 0;
    m_nativeTerminal.reset();
    m_pendingNativeContext.reset();
    m_pendingNativeParty = {};
    m_pendingNativeShop.reset();
    m_pendingNativeMovieVotes = 0;
    m_drainingInputs.reset();
    m_drainingAgreement.reset();
    m_drainingTerminal.reset();
    m_drainingContext.reset();
}
bool MatchSession::beginNativeInputs() {
    if (m_context.transition == MatchTransition::Shop) {
        return true;
    }
    auto inputs = std::make_unique<RollbackExchange>();
    std::vector<RollbackExchange::Link> links;
    std::vector<u8> absent;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_dropped[peer].peer != 0) {
            absent.push_back(peer); // its link is gone; its seats hold still from the start
        } else if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        } else if (m_direct[peer] != 0) {
            links.push_back({peer, m_direct[peer]});
        }
    }
    if (!inputs->open(m_local, m_context.epoch, m_owners, m_context.grants, m_context.inputLead,
                      links, {}, absent)) {
        return false;
    }
    m_exchange = std::move(inputs);
    m_nativeNextTick = 1;
    return true;
}
bool MatchSession::ensureNativeAgreement(Time now) {
    if (m_nativeAgreement) {
        return true;
    }
    auto agreement = std::make_unique<NativeAgreement>();
    std::vector<NativeAgreement::Link> links;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        }
    }
    if (!agreement->open(m_local, m_context.epoch, activeOwners(), links, kNativeAgreementPeriod,
                         now)) {
        failNativeAgreement(NativeAgreement::Failure::Protocol);
        return false;
    }
    m_nativeAgreement = std::move(agreement);
    return true;
}
bool MatchSession::ensureTerminalAgreement(u64 tick, Time now) {
    if (m_terminalAgreement) {
        return m_terminalAgreement->terminalTick() == tick;
    }
    auto agreement = std::make_unique<NativeAgreement>();
    std::vector<NativeAgreement::Link> links;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        }
    }
    if (!agreement->openTerminal(m_local, m_context.epoch, activeOwners(), links, tick, now)) {
        return false;
    }
    m_terminalAgreement = std::move(agreement);
    return true;
}
bool MatchSession::agreementFailed(Time now) {
    // Only peer one judges a mismatch, and only one repair runs at a time; anything
    // else about the stream's failure stays fatal.
    if (!host() || !m_nativeAgreement ||
        m_nativeAgreement->failure() != NativeAgreement::Failure::Mismatch || m_resync) {
        return false;
    }
    return beginResync(now);
}
bool MatchSession::beginResync(Time now) {
    Resync resync;
    resync.tick = m_nativeAgreement->failureTick();
    const auto& mismatched = m_nativeAgreement->mismatchedPeers();
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (mismatched[peer]) {
            resync.peers |= static_cast<u8>(1U << peer);
        }
    }
    if (resync.peers == 0 || resync.tick == 0) {
        return false;
    }
    // A rebuild that is followed at once by the same disagreement cannot help, nor can
    // repairs that keep coming: the play stops instead of rebuilding forever.
    for (u8 peer = 2; peer <= 4; ++peer) {
        if ((resync.peers & (1U << peer)) != 0 && m_rebuiltFrom[peer] != 0 &&
            resync.tick == m_rebuiltFrom[peer] + kNativeAgreementPeriod) {
            log::warn("Native rebuild of peer {} from tick {} did not bring it back into step",
                      peer, m_rebuiltFrom[peer]);
            return false;
        }
    }
    if (m_repairs >= kRepairsPerEpoch) {
        log::warn("Native observation mismatch at epoch {} tick {} after {} repairs",
                  m_context.epoch, resync.tick, m_repairs);
        return false;
    }
    if (!restartAgreement(resync.tick, now) ||
        !broadcast(MatchControlKind::Resync, MatchStop::None, 0, resync)) {
        return false;
    }
    log::warn("Native observation mismatch at epoch {} tick {}: peers {:#x} will rebuild",
              m_context.epoch, resync.tick, resync.peers);
    for (u8 peer = 2; peer <= 4; ++peer) {
        if ((resync.peers & (1U << peer)) != 0) {
            m_rebuiltFrom[peer] = resync.tick;
        }
    }
    ++m_repairs;
    m_resyncSince = now;
    m_resync = resync;
    m_consoleRequests.clear();
    m_journalQueuedPeers = 0;
    return true;
}
bool MatchSession::restartAgreement(u64 tick, Time now) {
    log::info("Native agreement restart: epoch={} peer={} restart={} next={} confirmed={} "
              "cached_future_digests={}",
              m_context.epoch, m_local, tick, m_nativeNextTick, m_nativeCompleted,
              std::ranges::count_if(m_recentDigests,
                                    [tick](const auto& value) { return value.first > tick; }));
    auto agreement = std::make_unique<NativeAgreement>();
    std::vector<NativeAgreement::Link> links;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            links.push_back({peer, m_links[peer]});
        }
    }
    if (!agreement->open(m_local, m_context.epoch, activeOwners(), links, kNativeAgreementPeriod,
                         now, tick)) {
        return false;
    }
    // Digests already taken past the restart are offered again to the new stream.
    for (const auto& [digestTick, digest] : m_recentDigests) {
        if (digestTick > tick &&
            agreement->submit(digestTick, digest, now) != NativeAgreement::Admission::Accepted) {
            return false;
        }
    }
    m_nativeAgreement = std::move(agreement);
    return true;
}
std::optional<MatchSession::Resync> MatchSession::pendingResync() const {
    if (!host() || !m_resync || (m_resync->peers & ~m_journalQueuedPeers) == 0) {
        return std::nullopt;
    }
    return Resync{m_resync->tick, static_cast<u8>(m_resync->peers & ~m_journalQueuedPeers)};
}
bool MatchSession::queueJournal(u8 peer, const JournalPacket::Chunk& chunk) {
    const auto bytes = JournalPacket::encode(chunk);
    if (!host() || !m_resync || peer == 0 || peer > 4 || m_links[peer] == 0 ||
        (m_resync->peers & (1U << peer)) == 0 || (m_journalQueuedPeers & (1U << peer)) != 0 ||
        !bytes || chunk.epoch != m_context.epoch ||
        m_journalDeliveries.size() >= kJournalDeliveries) {
        return false;
    }
    m_journalDeliveries.push_back({m_links[peer], *bytes});
    return true;
}
bool MatchSession::journalQueued(u8 peer) {
    if (!host() || !m_resync || peer == 0 || peer > 4 || (m_resync->peers & (1U << peer)) == 0) {
        return false;
    }
    m_journalQueuedPeers |= static_cast<u8>(1U << peer);
    return true;
}
std::optional<u64> MatchSession::resyncRequested() const {
    return m_resyncTarget && m_resync ? std::optional{m_resync->tick} : std::nullopt;
}
std::vector<InputCommand> MatchSession::takeJournal() {
    if (!journalReady()) {
        return {};
    }
    m_journalComplete = false;
    return std::exchange(m_journalIncoming, {});
}
bool MatchSession::finishResync() {
    if (!m_resyncTarget || !m_resync || host()) {
        return false;
    }
    Resync done;
    done.peers = static_cast<u8>(1U << m_local);
    if (!queue(1, MatchControlKind::ResyncDone, MatchStop::None, 0, done)) {
        return false;
    }
    log::info("Native resync acknowledged: epoch={} peer={} rebuilt={} prior_next={} "
              "prior_confirmed={} retained_local_inputs={}",
              m_context.epoch, m_local, m_resync->tick, m_nativeNextTick, m_nativeCompleted,
              m_exchange ? m_exchange->retainedFrames(m_local) : 0);
    m_resyncTarget = false;
    m_journalComplete = false;
    m_journalIncoming.clear();
    // The rebuilt scene confirms again from the resync tick onward.
    m_nativeCompleted = std::min(m_nativeCompleted, m_resync->tick);
    m_resync->peers &= static_cast<u8>(~done.peers);
    if (m_resync->peers == 0) {
        m_resync.reset();
    }
    return true;
}
MatchSession::Admission MatchSession::journal(u8 peer, const JournalPacket::Chunk& chunk) {
    if (host() || peer != 1) {
        return Admission::WrongPeer;
    }
    if (chunk.epoch != m_context.epoch || !m_resyncTarget || m_journalComplete) {
        return Admission::Stale;
    }
    if (m_journalIncoming.size() + chunk.commands.size() > kJournalCommands) {
        return Admission::Invalid;
    }
    m_journalIncoming.insert(m_journalIncoming.end(), chunk.commands.begin(), chunk.commands.end());
    m_journalComplete = chunk.last;
    return Admission::Accepted;
}
void MatchSession::failNativeAgreement(NativeAgreement::Failure fallback, u64 tick,
                                       const NativeAgreement* source) {
    const auto* agreement = source ? source : m_nativeAgreement.get();
    const auto reason = agreement && agreement->failed() ? agreement->failure() : fallback;
    if (agreement && agreement->failed()) {
        tick = agreement->failureTick();
    }
    if (m_phase != Phase::Stopped && m_phase != Phase::Offline) {
        m_failureDetail = "Simulation agreement failed: " + std::string(agreementReason(reason));
        if (tick != 0) {
            m_failureDetail += " at tick " + std::to_string(tick);
        }
        m_failureDetail += ". Online play stopped to protect progress.";
    }
    log::error("Native observation agreement failed: {} (epoch {}, tick {}, peer {})",
               agreementReason(reason), m_context.epoch, tick, m_local);
    // The existing shared Stop supersedes queued observation packets and is
    // reliably retained even when a normal output queue is congested.
    stop(MatchStop::TransportFailure, true);
}
bool MatchSession::acceptPreparedContext(const MatchContext& context, MatchParty party) {
    const auto sourceContext = m_context;
    auto sourceInputs = std::move(m_exchange);
    auto sourceAgreement = std::move(m_nativeAgreement);
    auto sourceTerminal = std::move(m_terminalAgreement);
    m_context = context;
    m_travelParty = std::move(party);
    m_shop.begin(m_context.epoch, m_local, m_owners, m_context.grants);
    for (const auto& drop : m_dropped) {
        if (drop.peer != 0) {
            m_shop.suspend(drop.peer);
        }
    }
    discardGameplay();
    if (!beginNativeInputs()) {
        stop(MatchStop::TransportFailure, false);
        return false;
    }
    m_drainingInputs = std::move(sourceInputs);
    m_drainingAgreement = std::move(sourceAgreement);
    m_drainingTerminal = std::move(sourceTerminal);
    if (m_drainingInputs || m_drainingAgreement) {
        m_drainingContext = sourceContext;
    }
    m_loaded = {};
    m_movieSkipVotes = droppedSeats(m_owners, m_dropped);
    m_movieSkipRequests = 0;
    m_loadingSeconds = 0;
    m_phase = Phase::Loading;
    return true;
}
bool MatchSession::finishNativeDrain() {
    if (!m_pendingNativeContext || m_nativeCompleted != m_pendingNativeContext->sourceTick ||
        (m_context.transition != MatchTransition::Shop &&
         m_nativeTerminal != m_pendingNativeContext->sourceTick)) {
        return false;
    }
    const auto context = *m_pendingNativeContext;
    auto party = std::move(m_pendingNativeParty);
    auto shop = std::move(m_pendingNativeShop);
    const auto votes = m_pendingNativeMovieVotes;
    if (!acceptPreparedContext(context, std::move(party))) {
        return false;
    }
    if (shop) {
        m_shop = std::move(*shop);
    }
    m_movieSkipVotes |= votes;
    return true;
}
bool MatchSession::prepare(u32 scene, MatchTransition transition, const MatchParty* party,
                           const ShopSetup* shop, bool challengeWon) {
    const bool withCheckpoint = checkpointed(transition);
    const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                         (withCheckpoint && m_phase == Phase::Running);
    if (!host() || !allowed || scene == 0 || scene > 65535 || m_pendingNativeContext ||
        (withCheckpoint && m_context.transition != MatchTransition::Shop &&
         (m_nativeCompleted == 0 || m_nativeTerminal != m_nativeCompleted)) ||
        withCheckpoint != (party != nullptr) ||
        (challengeWon && transition != MatchTransition::SecretReturn) ||
        (transition == MatchTransition::Shop) != (shop != nullptr) ||
        (shop != nullptr && (!shop->valid() || shop->source != scene)) ||
        m_context.epoch == std::numeric_limits<u64>::max()) {
        return false;
    }
    if (m_context.transition == MatchTransition::Shop &&
        (transition != MatchTransition::Travel || !m_shop.finished())) {
        return false;
    }
    std::vector<std::vector<u8>> profiles;
    if (party != nullptr) {
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if ((*party)[seat].has_value() != (m_owners[seat] != 0)) {
                return false;
            }
            if ((*party)[seat]) {
                auto packet = PartyCheckpointPacket::encode(
                    {m_context.epoch + 1, static_cast<u8>(seat), *(*party)[seat]});
                if (!packet) {
                    return false;
                }
                profiles.push_back(std::move(*packet));
            }
        }
    }
    const auto peers = static_cast<usize>(
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; }));
    if (m_controls.size() + peers * (1 + profiles.size()) > kControlBudget) {
        return false;
    }
    // Copy before replacing internal storage: callers may reuse travelParty().
    MatchParty nextParty = party != nullptr ? *party : MatchParty{};
    const auto previousContext = m_context;
    const auto finalTick = m_nativeCompleted;
    auto drainingInputs = withCheckpoint ? std::move(m_exchange) : nullptr;
    auto drainingAgreement = withCheckpoint ? std::move(m_nativeAgreement) : nullptr;
    auto drainingTerminal = withCheckpoint ? std::move(m_terminalAgreement) : nullptr;
    m_context = {};
    m_context.epoch = previousContext.epoch + 1;
    m_context.scene = scene;
    m_context.transition = transition;
    m_context.challengeWon = challengeWon;
    m_context.owners = m_owners;
    m_context.inputLead = m_startupDelay;
    if (withCheckpoint) {
        m_context.sourceEpoch = previousContext.epoch;
        m_context.sourceTick = finalTick;
    }
    m_travelParty = std::move(nextParty);
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        // Membership is fixed for this transport lifetime. Epochs, not grant
        // changes, isolate successive scenes; the original assigned grant is 1.
        m_context.grants[seat] = m_owners[seat] == 0 ? 0 : 1;
    }
    discardGameplay();
    if (!beginNativeInputs()) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_drainingInputs = std::move(drainingInputs);
    m_drainingAgreement = std::move(drainingAgreement);
    m_drainingTerminal = std::move(drainingTerminal);
    if (m_drainingInputs || m_drainingAgreement) {
        m_drainingContext = previousContext;
    }
    m_shop.begin(m_context.epoch, m_local, m_owners, m_context.grants, shop);
    for (const auto& drop : m_dropped) {
        if (drop.peer != 0) {
            m_shop.suspend(drop.peer);
        }
    }
    m_loaded = {};
    m_movieSkipVotes = droppedSeats(m_owners, m_dropped);
    m_movieSkipRequests = 0;
    m_loadingSeconds = 0;
    m_phase = Phase::Loading;
    if (!broadcast(MatchControlKind::Prepare)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    for (const auto connection : m_links) {
        if (connection != 0) {
            for (const auto& profile : profiles) {
                m_controls.push_back({connection, profile});
            }
        }
    }
    return true;
}
const MatchParty* MatchSession::travelParty() const {
    if (!checkpointed(m_context.transition) ||
        (m_phase != Phase::Loading && m_phase != Phase::Running)) {
        return nullptr;
    }
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_travelParty[seat].has_value() != (m_owners[seat] != 0)) {
            return nullptr;
        }
    }
    return &m_travelParty;
}
MatchSession::Admission MatchSession::checkpoint(u8 peer, const PartyCheckpoint& message) {
    if (host() || peer != 1) {
        return Admission::WrongPeer;
    }
    if (m_pendingNativeContext && message.epoch == m_pendingNativeContext->epoch) {
        if (m_owners[message.seat] == 0) {
            return Admission::Invalid;
        }
        auto& profile = m_pendingNativeParty[message.seat];
        if (profile) {
            return CharacterProfilePacket::encode(*profile) ==
                           CharacterProfilePacket::encode(message.profile)
                       ? Admission::Accepted
                       : Admission::Invalid;
        }
        profile = message.profile;
        return Admission::Accepted;
    }
    if (message.epoch < m_context.epoch) {
        return Admission::Stale;
    }
    if (message.epoch != m_context.epoch || !checkpointed(m_context.transition) ||
        m_owners[message.seat] == 0) {
        return Admission::Invalid;
    }
    auto& profile = m_travelParty[message.seat];
    if (profile) {
        return CharacterProfilePacket::encode(*profile) ==
                       CharacterProfilePacket::encode(message.profile)
                   ? Admission::Accepted
                   : Admission::Invalid;
    }
    if (m_phase != Phase::Loading || m_loaded[m_local]) {
        return Admission::WrongPhase;
    }
    profile = message.profile;
    return Admission::Accepted;
}
void MatchSession::tryCommit(Time at) {
    if (!host() || m_phase != Phase::Loading) {
        return;
    }
    for (const auto owner : activeOwners()) {
        if (owner != 0 && !m_loaded[owner]) {
            return;
        }
    }
    if (m_drainingAgreement &&
        (m_drainingAgreement->pendingTicks() != 0 || m_drainingAgreement->queuedPackets() != 0)) {
        return; // Final source hashes must finish before retiring their agreement stream.
    }
    if (m_drainingTerminal && (m_drainingTerminal->agreedTick() != m_context.sourceTick ||
                               m_drainingTerminal->queuedPackets() != 0)) {
        return;
    }
    if (!broadcast(MatchControlKind::Commit)) {
        stop(MatchStop::TransportFailure, true);
        return;
    }
    m_drainingInputs.reset();
    m_drainingAgreement.reset();
    m_drainingTerminal.reset();
    m_drainingContext.reset();
    m_phase = Phase::Running;
    hearEveryone(at);
}
void MatchSession::hearEveryone(Time at) {
    for (Time& heard : m_heardAt) {
        heard = std::max(heard, at); // a fresh word earlier in the same poll stands
    }
}
bool MatchSession::loaded() {
    if (m_phase != Phase::Loading || m_loaded[m_local] ||
        (m_context.transition != MatchTransition::Shop && !m_nativeConsumer) ||
        (checkpointed(m_context.transition) && travelParty() == nullptr) ||
        (m_context.transition == MatchTransition::Shop && m_shop.setup() == nullptr) ||
        (m_drainingTerminal && m_drainingTerminal->agreedTick() != m_context.sourceTick)) {
        return false;
    }
    m_loaded[m_local] = true;
    if (host()) {
        tryCommit(m_now);
    } else if (!queue(1, MatchControlKind::Ready)) {
        stop(MatchStop::TransportFailure, false);
        return false;
    }
    return true;
}
bool MatchSession::requestMovieSkip(u8 seat) {
    if (m_phase != Phase::Loading || !hasEntryMovie(m_context.transition) ||
        seat >= m_owners.size() || m_owners[seat] != m_local) {
        return false;
    }
    const auto bit = static_cast<u8>(1U << seat);
    if ((m_movieSkipRequests & bit) != 0) {
        return true;
    }
    const auto votes = static_cast<u8>(m_movieSkipVotes | bit);
    if (host() ? !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)
               : !queue(1, MatchControlKind::MovieSkipVote, MatchStop::None, bit)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_movieSkipRequests |= bit;
    if (host()) {
        m_movieSkipVotes = votes;
    }
    return true;
}
bool MatchSession::movieSkipReady() const {
    const auto required = seatsOf(m_owners);
    return m_phase == Phase::Loading && hasEntryMovie(m_context.transition) && required != 0 &&
           (m_movieSkipVotes & required) == required;
}
void MatchSession::stop(MatchStop reason, bool notify) {
    if (m_phase == Phase::Offline || m_phase == Phase::Stopped) {
        return;
    }
    if (m_failureDetail.empty()) {
        switch (reason) {
        case MatchStop::None: break;
        case MatchStop::Left: m_failureDetail = "A player left the online game."; break;
        case MatchStop::Disconnected:
            m_failureDetail = "A player disconnected. The online game has ended.";
            break;
        case MatchStop::LoadTimeout:
            m_failureDetail = "Timed out waiting for every player to load the next stage.";
            break;
        case MatchStop::TransportFailure:
            m_failureDetail = "The online connection failed. Play stopped to protect progress.";
            break;
        }
    }
    discardGameplay();
    m_controls.clear();
    m_shop.begin(0, 0, {}, {});
    m_phase = Phase::Stopped;
    m_stop = reason;
    if (notify && m_context.valid()) {
        if (host()) {
            broadcast(MatchControlKind::Stop, reason);
        } else {
            queue(1, MatchControlKind::Stop, reason);
        }
    }
}
void MatchSession::leave() {
    stop(MatchStop::Left, true);
}
MatchOwners MatchSession::activeOwners() const {
    MatchOwners owners = m_owners;
    for (auto& owner : owners) {
        if (owner != 0 && m_dropped[owner].peer != 0) {
            owner = 0;
        }
    }
    return owners;
}
void MatchSession::applyDrop(const Drop& drop) {
    m_dropped[drop.peer] = drop;
    m_pendingDrops.push_back(drop);
    m_loaded[drop.peer] = true;
    m_movieSkipVotes |= seatsOf(m_owners, drop.peer);
    // A dropped peer owes no rebuild, so nobody waits on its completion.
    const auto bit = static_cast<u8>(1U << drop.peer);
    m_journalQueuedPeers &= static_cast<u8>(~bit);
    if (m_resync) {
        m_resync->peers &= static_cast<u8>(~bit);
        if (m_resync->peers == 0) {
            m_resync.reset();
        }
    }
    for (auto* agreement : {m_nativeAgreement.get(), m_terminalAgreement.get(),
                            m_drainingAgreement.get(), m_drainingTerminal.get()}) {
        if (agreement) {
            agreement->suspend(drop.peer, m_now);
        }
    }
    for (auto* exchange : {m_exchange.get(), m_drainingInputs.get()}) {
        if (exchange) {
            exchange->suspend(drop.peer);
        }
    }
    m_shop.suspend(drop.peer);
    if (m_pendingNativeShop) {
        m_pendingNativeShop->suspend(drop.peer);
    }
    log::warn("Online peer {} dropped at epoch {} tick {}: its seats hold still", drop.peer,
              drop.epoch, drop.tick);
}
bool MatchSession::drop(u8 peer) {
    const bool playing =
        m_phase == Phase::Running || (m_phase == Phase::Loading && m_context.epoch > 1);
    if (!host() || peer < 2 || peer > 4 || m_dropped[peer].peer != 0 ||
        std::ranges::find(m_owners, peer) == m_owners.end() || !playing) {
        return false;
    }
    const u64 tick = m_context.transition == MatchTransition::Shop
                         ? m_shop.tick() + 1
                         : (m_exchange ? m_exchange->receivedThrough(peer) : 0) + 1;
    const Drop drop{peer, m_context.epoch, tick};
    // The control goes out on the link as it stands, the dropped peer's included.
    if (!broadcast(MatchControlKind::Drop, MatchStop::None, 0,
                   Resync{drop.tick, static_cast<u8>(1U << peer)})) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    applyDrop(drop);
    m_droppedLinks[peer] = m_links[peer];
    std::erase_if(m_journalDeliveries,
                  [&](const auto& queued) { return queued.connection == m_links[peer]; });
    m_links[peer] = 0;
    if (m_phase == Phase::Loading) {
        tryCommit(m_now);
    }
    return true;
}
std::vector<MatchSession::Drop> MatchSession::dropsFor(u64 epoch) const {
    std::vector<Drop> result;
    for (const auto& drop : m_dropped) {
        // A guest finishing a stage may already hold a drop the host made in the next one.
        if (drop.peer != 0 && drop.epoch <= epoch) {
            result.push_back({drop.peer, epoch, drop.epoch == epoch ? drop.tick : 1});
        }
    }
    return result;
}
bool MatchSession::shopLaneGone(u8 peer, u64 epoch, u64 step) const {
    const auto drops = dropsFor(epoch);
    return peer != 0 && std::ranges::any_of(drops, [&](const Drop& drop) {
               return drop.peer == peer && step + 1 >= drop.tick;
           });
}
std::vector<MatchSession::Drop> MatchSession::takeDrops() {
    return std::exchange(m_pendingDrops, {});
}
f64 MatchSession::silence(u8 peer, Time now) const {
    return now > m_heardAt[peer] ? std::chrono::duration<f64>(now - m_heardAt[peer]).count() : 0;
}
std::vector<u8> MatchSession::quietPeers(f64 seconds, Time now) const {
    std::vector<u8> result;
    if (m_phase != Phase::Running || !m_exchange) {
        return result;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        // A peer rebuilding owes nothing until it is done.
        if (peer == m_local || m_dropped[peer].peer != 0 ||
            std::ranges::find(m_owners, peer) == m_owners.end() ||
            (m_resync && (m_resync->peers & (1U << peer)) != 0)) {
            continue;
        }
        if (silence(peer, now) >= seconds) {
            result.push_back(peer);
        }
    }
    return result;
}
f64 MatchSession::quietSeconds(Time now) const {
    f64 longest = 0;
    for (const u8 peer : quietPeers(0, now)) {
        longest = std::max(longest, silence(peer, now));
    }
    return longest;
}
bool MatchSession::disconnected(PacketTransport::Connection connection) {
    const auto peer = peerOf(connection);
    if (peer == 0) {
        // A dropped guest's link closing after the drop, as it does when the guest left
        // on its own and its Stop arrived first, is nothing new.
        return droppedLink(m_droppedLinks, connection);
    }
    // Nothing queued for a closed link can leave, and it must not crowd out what the others
    // are still owed, the drop's own notice among it.
    const auto closed = [&](const auto& queued) { return queued.connection == connection; };
    std::erase_if(m_controls, closed);
    std::erase_if(m_journalDeliveries, closed);
    if (m_dropped[peer].peer != 0 || (host() && peer != 1 && drop(peer))) {
        m_links[peer] = 0;
        std::erase_if(m_controls, closed);
        return true;
    }
    if (m_nativeAgreement) {
        m_nativeAgreement->disconnected(connection);
    }
    if (m_drainingAgreement) {
        m_drainingAgreement->disconnected(connection);
    }
    if (m_terminalAgreement) {
        m_terminalAgreement->disconnected(connection);
    }
    if (m_drainingTerminal) {
        m_drainingTerminal->disconnected(connection);
    }
    if (m_exchange) {
        m_exchange->disconnected(connection);
    }
    if (m_drainingInputs) {
        m_drainingInputs->disconnected(connection);
    }
    m_links[peer] = 0;
    std::erase_if(m_controls, closed);
    std::erase_if(m_journalDeliveries, closed);
    if (m_nativeAgreement || m_terminalAgreement || m_drainingTerminal || m_drainingAgreement) {
        const auto* agreement = m_nativeAgreement     ? m_nativeAgreement.get()
                                : m_terminalAgreement ? m_terminalAgreement.get()
                                : m_drainingTerminal  ? m_drainingTerminal.get()
                                                      : m_drainingAgreement.get();
        failNativeAgreement(NativeAgreement::Failure::Disconnected, 0, agreement);
    } else {
        stop(MatchStop::Disconnected, host());
    }
    return false;
}
void MatchSession::dropOverdue(Time now) {
    for (const auto* stream :
         {&m_terminalAgreement, &m_drainingTerminal, &m_drainingAgreement, &m_nativeAgreement}) {
        // Read again each time: a drop can retire a draining stream.
        const NativeAgreement* agreement = stream->get();
        if (agreement == nullptr) {
            continue;
        }
        for (const u8 peer : agreement->overdue(now)) {
            const bool rebuilding = m_resync && (m_resync->peers & (1U << peer)) != 0;
            if (!rebuilding && !dropped(peer)) {
                log::warn("Online peer {} is overdue at epoch {}: dropping it", peer,
                          m_context.epoch);
                drop(peer);
            }
        }
        if (m_phase == Phase::Stopped) {
            return;
        }
    }
    if (!repairOverdue(now)) {
        return;
    }
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (m_resync && (m_resync->peers & (1U << peer)) != 0) {
            log::warn("Online peer {} did not finish its rebuild from tick {}: dropping it", peer,
                      m_resync->tick);
            drop(peer);
        }
    }
}
f64 MatchSession::repairAllowance() const {
    const f64 allowance =
        kRepairSeconds + kRepairSecondsPerTick * static_cast<f64>(m_resync ? m_resync->tick : 0);
    return host() ? allowance
                  : allowance + std::chrono::duration<f64>(NativeAgreement::kGuestTimeout).count();
}
bool MatchSession::repairOverdue(Time now) const {
    return m_resync && now >= m_resyncSince &&
           std::chrono::duration<f64>(now - m_resyncSince).count() >= repairAllowance();
}
bool MatchSession::dropUnloaded() {
    // The first stage, or one the host has not loaded itself, has nobody to drop.
    if (!host() || m_phase != Phase::Loading || m_context.epoch <= 1 || !m_loaded[m_local]) {
        return false;
    }
    for (const u8 peer : activeOwners()) {
        if (peer > 1 && !m_loaded[peer] && !dropped(peer)) {
            log::warn("Online peer {} has not loaded epoch {} in time: dropping it", peer,
                      m_context.epoch);
            if (!drop(peer)) {
                return false;
            }
        }
    }
    return m_phase != Phase::Stopped;
}
void MatchSession::update(f64 seconds, Time now) {
    m_now = std::max(m_now, now);
    if (host()) {
        dropOverdue(now);
        if (m_phase == Phase::Stopped) {
            return;
        }
    }
    if (m_resync) {
        // A guest leaves a repair the host never ends. Until it ends the repair holds every
        // stream: nobody owes anything meanwhile.
        if (!host() && repairOverdue(now)) {
            log::warn("Online repair from tick {} outlasted the host's allowance: leaving",
                      m_resync->tick);
            m_failureDetail = "The host did not finish repairing the online game in time. "
                              "Online play stopped to protect progress.";
            stop(MatchStop::TransportFailure, true);
            return;
        }
        for (auto* agreement : {m_nativeAgreement.get(), m_terminalAgreement.get(),
                                m_drainingAgreement.get(), m_drainingTerminal.get()}) {
            if (agreement) {
                agreement->rearm(now);
            }
        }
    }
    for (auto* agreement : {m_terminalAgreement.get(), m_drainingTerminal.get()}) {
        if (agreement && !agreement->update(now)) {
            failNativeAgreement(NativeAgreement::Failure::Timeout, 0, agreement);
            return;
        }
    }
    if (m_drainingAgreement && !m_drainingAgreement->update(now)) {
        failNativeAgreement(NativeAgreement::Failure::Timeout, 0, m_drainingAgreement.get());
        return;
    }
    // A drop can leave the remaining digests disagreeing; that is repaired like any mismatch.
    if (m_nativeAgreement && !m_nativeAgreement->update(now) && !agreementFailed(now)) {
        failNativeAgreement(NativeAgreement::Failure::Timeout);
        return;
    }
    if ((m_phase == Phase::Loading || m_pendingNativeContext) && std::isfinite(seconds) &&
        seconds > 0) {
        m_loadingSeconds += seconds;
        if (m_loadingSeconds >= (host() ? kLoadTimeout : kGuestLoadTimeout) && !dropUnloaded()) {
            stop(MatchStop::LoadTimeout, true);
        }
    }
}
MatchSession::Admission MatchSession::control(u8 peer, const MatchControl& message, Time inputAt) {
    if (message.context.inputLead != m_startupDelay) {
        return Admission::Invalid;
    }
    // A host can abort before its queued Prepare reaches a guest. Stop is final,
    // not a request to start that future epoch or load its scene.
    if (!host() && peer == 1 && message.kind == MatchControlKind::Stop &&
        message.context.owners == m_owners && message.context.epoch >= m_context.epoch) {
        stop(message.stop, false);
        return Admission::Accepted;
    }
    if (host() && m_drainingContext && message.kind == MatchControlKind::Stop &&
        message.context == *m_drainingContext) {
        stop(message.stop == MatchStop::TransportFailure ? MatchStop::TransportFailure
                                                         : MatchStop::Left,
             true);
        return Admission::Accepted;
    }
    if (message.kind == MatchControlKind::Prepare) {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (message.context.epoch <= m_context.epoch) {
            return Admission::Stale;
        }
        if (message.context.owners != m_owners) {
            return Admission::Invalid;
        }
        if (m_pendingNativeContext) {
            return message.context == *m_pendingNativeContext ? Admission::Stale
                                                              : Admission::Invalid;
        }
        const auto transition = message.context.transition;
        const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                             (checkpointed(transition) && m_phase == Phase::Running);
        if (!allowed || (m_context.transition == MatchTransition::Shop &&
                         transition != MatchTransition::Travel)) {
            return Admission::WrongPhase;
        }
        if (checkpointed(transition)) {
            if (message.context.sourceEpoch != m_context.epoch ||
                message.context.sourceTick < m_nativeCompleted ||
                message.context.sourceTick - m_nativeCompleted > RollbackInputs::kMaxAhead ||
                (m_context.transition == MatchTransition::Shop ? message.context.sourceTick != 0
                                                               : message.context.sourceTick == 0)) {
                return Admission::Invalid;
            }
            m_pendingNativeContext = message.context;
            m_pendingNativeParty = {};
            m_loadingSeconds = 0;
            if (transition == MatchTransition::Shop) {
                m_pendingNativeShop = std::make_unique<ShopSync>();
                m_pendingNativeShop->begin(message.context.epoch, m_local, m_owners,
                                           message.context.grants);
                for (const auto& drop : m_dropped) {
                    if (drop.peer != 0) {
                        m_pendingNativeShop->suspend(drop.peer);
                    }
                }
            }
            // Even an already-executed boundary needs the scene adapter's
            // terminal-outcome check before acknowledging travel. A Shop has
            // no native scene to execute and retains its ordinary barrier.
            if (m_context.transition == MatchTransition::Shop) {
                return finishNativeDrain() ? Admission::Accepted : Admission::Invalid;
            }
            return Admission::Accepted;
        }
        return acceptPreparedContext(message.context) ? Admission::Accepted : Admission::Invalid;
    }
    if (m_pendingNativeContext && message.context == *m_pendingNativeContext && peer == 1 &&
        message.kind == MatchControlKind::MovieSkipVote) {
        m_pendingNativeMovieVotes |= message.movieSkipVotes;
        return Admission::Accepted;
    }
    // A drop the host makes once it has prepared the next stage can reach a guest still
    // finishing this one; it belongs to that stage, as the host's own does.
    const bool pendingDrop = message.kind == MatchControlKind::Drop && m_pendingNativeContext &&
                             message.context == *m_pendingNativeContext;
    if (message.context != m_context && !pendingDrop) {
        return Admission::Stale;
    }
    switch (message.kind) {
    case MatchControlKind::MovieSkipVote: {
        if (host() && (message.movieSkipVotes & ~seatsOf(m_owners, peer)) != 0) {
            return Admission::WrongPeer;
        }
        // A late vote may follow natural movie completion and the final Ready.
        if (m_phase != Phase::Loading) {
            return Admission::Stale;
        }
        const auto votes = static_cast<u8>(m_movieSkipVotes | message.movieSkipVotes);
        if (host() && votes != m_movieSkipVotes &&
            !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)) {
            stop(MatchStop::TransportFailure, true);
            return Admission::Accepted;
        }
        m_movieSkipVotes = votes;
        return Admission::Accepted;
    }
    case MatchControlKind::Ready:
        if (!host()) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading) {
            return Admission::WrongPhase;
        }
        m_loaded[peer] = true;
        tryCommit(inputAt); // the last word the barrier waited on came just now
        return Admission::Accepted;
    case MatchControlKind::Commit:
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading || !m_loaded[m_local] ||
            (m_drainingTerminal && m_drainingTerminal->agreedTick() != m_context.sourceTick)) {
            return Admission::WrongPhase;
        }
        m_phase = Phase::Running;
        m_drainingInputs.reset();
        m_drainingAgreement.reset();
        m_drainingTerminal.reset();
        m_drainingContext.reset();
        hearEveryone(inputAt);
        return Admission::Accepted;
    case MatchControlKind::Resync: {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Running) {
            return Admission::WrongPhase;
        }
        const bool target = (message.resyncPeers & (1U << m_local)) != 0;
        if (target) {
            // The rebuilt scene hashes those ticks again; what the diverged one took is wrong.
            std::erase_if(m_recentDigests,
                          [&](const auto& recent) { return recent.first > message.resyncTick; });
        }
        if (!restartAgreement(message.resyncTick, inputAt)) {
            failNativeAgreement(NativeAgreement::Failure::Protocol, message.resyncTick);
            return Admission::Accepted;
        }
        m_resync = Resync{message.resyncTick, message.resyncPeers};
        m_resyncSince = m_now;
        m_consoleRequests.clear();
        m_resyncTarget = target;
        m_journalComplete = false;
        m_journalIncoming.clear();
        return Admission::Accepted;
    }
    case MatchControlKind::ResyncDone: {
        if (!m_resync || (host() ? (message.resyncPeers & (1U << peer)) == 0 : peer != 1)) {
            return Admission::Stale;
        }
        if (host() && !broadcast(MatchControlKind::ResyncDone, MatchStop::None, 0,
                                 Resync{0, message.resyncPeers})) {
            stop(MatchStop::TransportFailure, true);
            return Admission::Accepted;
        }
        for (u8 rebuilt = 1; rebuilt <= 4; ++rebuilt) {
            if ((message.resyncPeers & (1U << rebuilt)) != 0) {
                m_heardAt[rebuilt] = m_now; // its silence while it rebuilt was owed
            }
        }
        m_journalQueuedPeers &= static_cast<u8>(~message.resyncPeers);
        m_resync->peers &= static_cast<u8>(~message.resyncPeers);
        if (m_resync->peers == 0) {
            m_resync.reset();
        }
        return Admission::Accepted;
    }
    case MatchControlKind::Drop: {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        for (u8 dropped = 2; dropped <= 4; ++dropped) {
            if ((message.resyncPeers & (1U << dropped)) == 0) {
                continue;
            }
            if (m_dropped[dropped].peer != 0) {
                return Admission::Stale;
            }
            if (dropped == m_local) {
                m_failureDetail = "You were dropped from the online game for not responding.";
                stop(MatchStop::Left, false);
                return Admission::Accepted;
            }
            applyDrop({dropped, message.context.epoch, message.resyncTick});
        }
        return Admission::Accepted;
    }
    case MatchControlKind::Pause:
    case MatchControlKind::RequestPause: return Admission::Invalid;
    case MatchControlKind::Stop: {
        const auto link = m_links[peer];
        if (host() && peer != 1 && drop(peer)) {
            // A guest that leaves on its own is dropped; nothing more goes to it.
            std::erase_if(m_controls,
                          [&](const auto& queued) { return queued.connection == link; });
            return Admission::Accepted;
        }
        stop(host() && message.stop != MatchStop::TransportFailure ? MatchStop::Left : message.stop,
             host());
        return Admission::Accepted;
    }
    case MatchControlKind::Prepare: break;
    }
    return Admission::Invalid;
}
MatchSession::Admission MatchSession::receive(PacketTransport::Connection connection,
                                              std::span<const u8> bytes, std::optional<Time> now,
                                              s32 pingMs) {
    const auto inputAt = now.value_or(Clock::now());
    const auto peer = peerOf(connection);
    if (peer == 0) {
        // Whatever a dropped guest sent before it heard of its drop is ignored, not refused.
        if (droppedLink(m_droppedLinks, connection)) {
            return Admission::Stale;
        }
        // A direct link carries another guest's own inputs and nothing else.
        if (directPeerOf(connection) == 0 || !RollbackExchange::recognizes(bytes)) {
            return Admission::WrongPeer;
        }
        if (m_phase == Phase::Stopped || m_phase == Phase::Offline) {
            return Admission::WrongPhase;
        }
        return nativeInputs(connection, bytes, pingMs, inputAt);
    }
    if (m_phase == Phase::Stopped || m_phase == Phase::Offline) {
        return Admission::WrongPhase;
    }
    if (bytes.size() >= 4 && ByteReader(bytes).readU32() == kConsoleMagic) {
        return consoleMessage(peer, bytes);
    }
    if (bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic) {
        const auto message = MatchControlPacket::decode(bytes);
        return message ? control(peer, *message, inputAt) : Admission::Invalid;
    }
    if (NativeAgreement::recognizesTerminal(bytes)) {
        const auto terminal = NativeAgreement::terminalContext(bytes);
        if (!terminal) {
            failNativeAgreement(NativeAgreement::Failure::Protocol);
            return Admission::Invalid;
        }
        const auto [epoch, tick] = *terminal;
        NativeAgreement* agreement = nullptr;
        if (m_drainingContext && epoch == m_drainingContext->epoch) {
            agreement = m_drainingTerminal.get();
            if (!agreement) {
                return Admission::Stale;
            }
        } else if (epoch == m_context.epoch && m_phase == Phase::Running &&
                   m_context.transition != MatchTransition::Shop) {
            // A validated epoch-wide failure has no scheduled tick. Route it
            // to an existing stream, or stop the current epoch without opening
            // a fictitious tick-zero certificate. Old-epoch failures still take
            // the stale branch below, after the retained source is retired.
            if (tick == 0 && !m_terminalAgreement) {
                failNativeAgreement(NativeAgreement::Failure::Protocol);
                return Admission::Accepted;
            }
            if (tick != 0 &&
                (tick < m_nativeCompleted ||
                 (tick > m_nativeNextTick && tick - m_nativeNextTick > RollbackInputs::kMaxAhead) ||
                 !ensureTerminalAgreement(tick, inputAt))) {
                failNativeAgreement(NativeAgreement::Failure::Protocol, tick);
                return Admission::Invalid;
            }
            agreement = m_terminalAgreement.get();
        } else {
            return Admission::Stale;
        }
        const auto result = agreement->receive(connection, bytes, inputAt);
        if (agreement->failed()) {
            failNativeAgreement(NativeAgreement::Failure::Protocol, tick, agreement);
            return result == NativeAgreement::Admission::Invalid ? Admission::Invalid
                                                                 : Admission::Accepted;
        }
        if (result == NativeAgreement::Admission::WrongPeer) {
            return Admission::WrongPeer;
        }
        return result == NativeAgreement::Admission::Accepted ||
                       result == NativeAgreement::Admission::Duplicate
                   ? Admission::Accepted
                   : Admission::Stale;
    }
    if (NativeAgreement::recognizes(bytes)) {
        if (m_drainingAgreement) {
            const auto result = m_drainingAgreement->receive(connection, bytes, inputAt);
            if (m_drainingAgreement->failed()) {
                failNativeAgreement(NativeAgreement::Failure::Protocol, 0,
                                    m_drainingAgreement.get());
                return result == NativeAgreement::Admission::Invalid ? Admission::Invalid
                                                                     : Admission::Accepted;
            }
            if (result != NativeAgreement::Admission::Stale) {
                switch (result) {
                case NativeAgreement::Admission::Accepted:
                case NativeAgreement::Admission::Duplicate: return Admission::Accepted;
                case NativeAgreement::Admission::WrongPeer: return Admission::WrongPeer;
                default: return Admission::Invalid;
                }
            }
        }
        if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop) {
            return Admission::Stale;
        }
        if (host() && m_resync && (m_resync->peers & (1U << peer)) != 0) {
            // Until its completion arrives a rebuilding peer reports the timeline that
            // diverged; the restarted stream takes its digests only after that. A failure
            // it reports meanwhile comes with its Stop as well.
            return Admission::Stale;
        }
        const bool fresh = !m_nativeAgreement;
        if (!ensureNativeAgreement(inputAt)) {
            return Admission::Invalid;
        }
        const auto result = m_nativeAgreement->receive(connection, bytes, inputAt);
        if (m_nativeAgreement->failed() && agreementFailed(inputAt)) {
            return Admission::Accepted;
        }
        if (m_nativeAgreement->failed()) {
            failNativeAgreement(NativeAgreement::Failure::Protocol);
            // A validated divergence notification is handled, not a request for
            // repair. Stop is already queued to terminate the whole shared match.
            return result == NativeAgreement::Admission::Invalid ? Admission::Invalid
                                                                 : Admission::Accepted;
        }
        switch (result) {
        case NativeAgreement::Admission::Accepted:
        case NativeAgreement::Admission::Duplicate: return Admission::Accepted;
        case NativeAgreement::Admission::Stale:
            if (fresh) {
                m_nativeAgreement.reset(); // late old epochs do not enable this epoch's observer
            }
            return Admission::Stale;
        case NativeAgreement::Admission::WrongPeer: return Admission::WrongPeer;
        case NativeAgreement::Admission::Full:
        case NativeAgreement::Admission::Failed:
            failNativeAgreement(NativeAgreement::Failure::Window);
            return Admission::Invalid;
        case NativeAgreement::Admission::Invalid: return Admission::Invalid;
        }
        return Admission::Invalid;
    }
    if (JournalPacket::recognizes(bytes)) {
        const auto chunk = JournalPacket::decode(bytes);
        return chunk ? journal(peer, *chunk) : Admission::Invalid;
    }
    if (PartyCheckpointPacket::recognizes(bytes)) {
        const auto message = PartyCheckpointPacket::decode(bytes);
        return message ? checkpoint(peer, *message) : Admission::Invalid;
    }
    if (ShopSync::recognizes(bytes)) {
        if (!m_pendingNativeShop && m_context.transition != MatchTransition::Shop) {
            return Admission::Stale;
        }
        // A future setup may follow Prepare while the old scene still drains.
        // The separate bounded ShopSync cannot mutate its profiles or gameplay.
        auto& shop = m_pendingNativeShop ? *m_pendingNativeShop : m_shop;
        switch (shop.receive(peer, bytes)) {
        case ShopSync::Admission::Accepted: return Admission::Accepted;
        case ShopSync::Admission::Stale: return Admission::Stale;
        case ShopSync::Admission::WrongPeer: return Admission::WrongPeer;
        case ShopSync::Admission::Invalid: return Admission::Invalid;
        }
    }
    if (RollbackExchange::recognizes(bytes)) {
        return nativeInputs(connection, bytes, pingMs, inputAt);
    }
    return Admission::Invalid; // Legacy bare input packets cannot enter this protocol.
}
MatchSession::Admission MatchSession::nativeInputs(PacketTransport::Connection connection,
                                                   std::span<const u8> bytes, s32 pingMs,
                                                   Time inputAt) {
    const auto message = RollbackExchange::decode(bytes);
    if (!message) {
        return Admission::Invalid;
    }
    auto* inputs = m_exchange.get();
    bool draining = false;
    if (m_drainingInputs && m_drainingContext && message->epoch == m_drainingContext->epoch) {
        inputs = m_drainingInputs.get();
        draining = true;
    } else if (message->epoch != m_context.epoch || !inputs || !m_nativeConsumer ||
               m_context.transition == MatchTransition::Shop ||
               (m_phase != Phase::Running && (m_phase != Phase::Loading || !m_loaded[m_local]))) {
        return Admission::Stale;
    }
    // A redundant packet can contain the missing terminal frame and later
    // sampled frames together. Validate the whole authenticated journal;
    // the scene's confirmed fence, not packet filtering, prevents execution.
    // Fresh progress, whoever relays it, is what shows a peer is still there.
    const auto result =
        inputs->receive(connection, bytes, pingMs,
                        [&](const RollbackExchange::Message& value, std::optional<u8> transit) {
                            if (draining) {
                                return true;
                            }
                            if (!m_nativeConsumer(value, transit)) {
                                return false;
                            }
                            if (transit && value.origin < m_heardAt.size()) {
                                m_heardAt[value.origin] = inputAt;
                            }
                            return true;
                        });
    switch (result) {
    case RollbackExchange::Admission::Accepted: return Admission::Accepted;
    case RollbackExchange::Admission::WrongEpoch: return Admission::Stale;
    case RollbackExchange::Admission::WrongPeer: return Admission::WrongPeer;
    case RollbackExchange::Admission::Full:
    case RollbackExchange::Admission::Conflict:
    case RollbackExchange::Admission::Rejected:
        // A callback may have admitted a prefix before another native guard
        // refused. Stop rather than retrying an ambiguous partially consumed batch.
        m_failureDetail = std::format("Online input reception failed: {} (epoch {}, peer {}, "
                                      "origin {}, next tick {}). Play stopped to protect progress.",
                                      inputAdmissionReason(result), m_context.epoch, m_local,
                                      message->origin, m_nativeNextTick);
        log::error("{} sender_next={} commands={} confirmed={} received_through={} "
                   "retained_frames={} resync={} draining={} rtt_ms={}",
                   m_failureDetail, message->nextTick, message->commands.size(), m_nativeCompleted,
                   inputs->receivedThrough(message->origin),
                   inputs->retainedFrames(message->origin), m_resync.has_value(), draining, pingMs);
        stop(MatchStop::TransportFailure, true);
        return Admission::Invalid;
    case RollbackExchange::Admission::Invalid: return Admission::Invalid;
    }
    return Admission::Invalid;
}
bool MatchSession::attachNativeInputs(u64 epoch, NativeInputConsumer consumer) {
    // A rebuilt scene attaches again while running; nothing else does after loading.
    const bool loading = m_phase == Phase::Loading && !m_loaded[m_local];
    const bool rebuilding = m_phase == Phase::Running && m_resyncTarget;
    if ((!loading && !rebuilding) || epoch != m_context.epoch || !m_exchange || m_nativeConsumer ||
        !consumer) {
        return false;
    }
    m_nativeConsumer = std::move(consumer);
    return true;
}
void MatchSession::detachNativeInputs(u64 epoch) {
    if (epoch == m_context.epoch) {
        m_nativeConsumer = {};
    }
}
bool MatchSession::publishNativeInputs(std::span<const InputCommand> commands, u64 nextTick) {
    if (m_phase != Phase::Running || !m_nativeConsumer || !m_exchange || nextTick == 0 ||
        nextTick <= m_nativeCompleted || (m_nativeTerminal && nextTick != *m_nativeTerminal + 1)) {
        return false;
    }
    if (!m_exchange->publish(commands, nextTick)) {
        m_failureDetail = std::format("Online input publication failed: {} (epoch {}, peer {}, "
                                      "next tick {}). Play stopped to protect progress.",
                                      inputFailureReason(m_exchange->failure()), m_context.epoch,
                                      m_local, nextTick);
        log::error("{} commands={} confirmed={} received_through={} retained_frames={} resync={}",
                   m_failureDetail, commands.size(), m_nativeCompleted,
                   m_exchange->receivedThrough(m_local), m_exchange->retainedFrames(m_local),
                   m_resync.has_value());
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_nativeNextTick = nextTick;
    return true;
}
bool MatchSession::confirmNativeTick(u64 epoch, u64 through) {
    if (m_phase != Phase::Running || epoch != m_context.epoch || !m_nativeConsumer || !m_exchange ||
        through < m_nativeCompleted || through >= m_nativeNextTick ||
        (m_pendingNativeContext && through > m_pendingNativeContext->sourceTick)) {
        return false;
    }
    for (const auto owner : activeOwners()) {
        if (owner != 0 && m_exchange->receivedThrough(owner) < through) {
            return false;
        }
    }
    m_nativeCompleted = through;
    return true;
}
bool MatchSession::completeNativeBoundary(u64 epoch, u64 tick) {
    if (m_phase != Phase::Running || epoch != m_context.epoch || !m_nativeConsumer || !m_exchange ||
        tick == 0 || tick != m_nativeCompleted || tick == std::numeric_limits<u64>::max() ||
        tick + 1 != m_nativeNextTick || !nativeTerminalAgreed(epoch, tick) ||
        (m_nativeTerminal && *m_nativeTerminal != tick) ||
        (m_pendingNativeContext && m_pendingNativeContext->sourceTick != tick)) {
        return false;
    }
    m_nativeTerminal = tick;
    return !m_pendingNativeContext || finishNativeDrain();
}
bool MatchSession::observeNativeTerminal(u64 epoch, u64 tick, const NativeAgreement::Digest& digest,
                                         Time now) {
    if (m_phase != Phase::Running || epoch != m_context.epoch || !m_nativeConsumer || tick == 0 ||
        tick != m_nativeCompleted || tick == std::numeric_limits<u64>::max() ||
        tick + 1 != m_nativeNextTick) {
        return false;
    }
    if (!ensureTerminalAgreement(tick, now)) {
        failNativeAgreement(NativeAgreement::Failure::Protocol, tick);
        return false;
    }
    const auto admitted = m_terminalAgreement->submit(tick, digest, now);
    if (admitted == NativeAgreement::Admission::Accepted ||
        admitted == NativeAgreement::Admission::Duplicate) {
        return true;
    }
    failNativeAgreement(NativeAgreement::Failure::Protocol, tick, m_terminalAgreement.get());
    return false;
}
bool MatchSession::nativeTerminalAgreed(u64 epoch, u64 tick) const {
    if (tick == 0) {
        return false;
    }
    if (epoch == m_context.epoch) {
        return m_terminalAgreement && !m_terminalAgreement->failed() &&
               m_terminalAgreement->agreedTick() == tick;
    }
    return m_drainingContext && epoch == m_drainingContext->epoch && m_drainingTerminal &&
           !m_drainingTerminal->failed() && m_drainingTerminal->agreedTick() == tick;
}
bool MatchSession::observeNative(u64 tick, const NativeAgreement::Digest& digest, Time now) {
    if (m_phase != Phase::Running || m_context.transition == MatchTransition::Shop || tick == 0 ||
        tick > m_nativeCompleted || tick % kNativeAgreementPeriod != 0) {
        return false;
    }
    if (!ensureNativeAgreement(now)) {
        return false;
    }
    m_recentDigests[tick] = digest;
    while (m_recentDigests.size() > kRecentDigests) {
        m_recentDigests.erase(m_recentDigests.begin());
    }
    switch (m_nativeAgreement->submit(tick, digest, now)) {
    case NativeAgreement::Admission::Accepted:
    case NativeAgreement::Admission::Duplicate:
        return !m_nativeAgreement->failed() || agreementFailed(now);
    case NativeAgreement::Admission::Full:
        failNativeAgreement(NativeAgreement::Failure::Window, tick);
        return false;
    case NativeAgreement::Admission::Failed:
        // The host's own digest can be the one that completes a mismatch; that is repaired
        // as when a report completes it.
        if (agreementFailed(now)) {
            return true;
        }
        failNativeAgreement(NativeAgreement::Failure::Protocol, tick);
        return false;
    case NativeAgreement::Admission::Stale:
    case NativeAgreement::Admission::WrongPeer:
    case NativeAgreement::Admission::Invalid:
        failNativeAgreement(NativeAgreement::Failure::Protocol, tick);
        return false;
    }
    return false;
}
void MatchSession::flush(PacketTransport& transport, Time now) {
    PacketTransport::Connection closedConnection = 0;
    // A final source verdict precedes Prepare/Ready on the same reliable link.
    for (auto* agreement : {m_terminalAgreement.get(), m_drainingTerminal.get()}) {
        if (agreement && !agreement->flush(transport, now, &closedConnection)) {
            if (closedConnection != 0) {
                disconnected(closedConnection);
                return;
            }
            failNativeAgreement(NativeAgreement::Failure::Transport, 0, agreement);
            return;
        }
        if (agreement && agreement->queuedPackets() != 0) {
            return;
        }
    }
    // These reliable reports precede Ready on the same connection. A peer may
    // already be loading the destination, but its last confirmed source hash
    // must not disappear when the source scene/consumer is released.
    if (m_drainingAgreement) {
        if (!m_drainingAgreement->flush(transport, now, &closedConnection)) {
            if (closedConnection != 0) {
                disconnected(closedConnection);
                return;
            }
            failNativeAgreement(NativeAgreement::Failure::Transport, 0, m_drainingAgreement.get());
            return;
        }
        if (m_drainingAgreement->queuedPackets() != 0) {
            return;
        }
    }
    tryCommit(now);
    while (!m_controls.empty()) {
        const auto& control = m_controls.front();
        const auto result =
            transport.send(control.connection, control.bytes, PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return;
        }
        if (result != PacketTransport::SendResult::Sent) {
            const auto connection = control.connection;
            if (peerOf(connection) == 0) {
                m_controls.pop_front(); // that link closed after this was queued
                continue;
            }
            disconnected(connection);
            return;
        }
        m_controls.pop_front();
    }
    while (!m_journalDeliveries.empty()) {
        const auto& delivery = m_journalDeliveries.front();
        const auto result = transport.send(delivery.connection, delivery.bytes,
                                           PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return;
        }
        if (result != PacketTransport::SendResult::Sent) {
            const auto connection = delivery.connection;
            if (peerOf(connection) == 0) {
                m_journalDeliveries.pop_front(); // that link closed after this was queued
                continue;
            }
            disconnected(connection);
            return;
        }
        m_journalDeliveries.pop_front();
    }
    // Prepare may reach a guest before old input deliveries. Retain and drain
    // the source relay throughout Loading; Ready acknowledges execution of its
    // terminal tick, so tryCommit() is the only successful retirement boundary.
    if (m_drainingInputs && !m_drainingInputs->flush(transport, &closedConnection)) {
        if (closedConnection != 0) {
            disconnected(closedConnection);
            return;
        }
        stop(MatchStop::TransportFailure, true);
        return;
    }
    if (m_context.transition == MatchTransition::Shop) {
        while (!m_shop.outgoing().empty()) {
            const auto& delivery = m_shop.outgoing().front();
            const auto connection = m_links[delivery.peer];
            if (connection == 0) {
                m_shop.sent(); // a dropped guest's link is gone: nothing more goes to it
                continue;
            }
            const auto result =
                transport.send(connection, delivery.bytes, PacketTransport::Delivery::Reliable);
            if (result == PacketTransport::SendResult::Congested) {
                return;
            }
            if (result != PacketTransport::SendResult::Sent) {
                disconnected(connection);
                return;
            }
            m_shop.sent();
        }
        return;
    }
    if (m_phase != Phase::Running && (m_phase != Phase::Loading || !m_loaded[m_local])) {
        return;
    }
    // In particular, a congested Commit above must reach this destination
    // before any new input publication. The unreliable stream independently
    // retains ACKed/redundant actual input while every native scene owns replay.
    if (!m_exchange || !m_exchange->flush(transport, &closedConnection)) {
        if (closedConnection != 0) {
            disconnected(closedConnection);
            return;
        }
        stop(MatchStop::TransportFailure, true);
        return;
    }
    // A mismatch is repaired before the failed stream's notice can leave; the order to rebuild
    // then goes out ahead of the restarted stream.
    if (m_nativeAgreement && m_nativeAgreement->failed() && agreementFailed(now)) {
        return;
    }
    if (m_nativeAgreement && !m_nativeAgreement->flush(transport, now, &closedConnection)) {
        if (closedConnection != 0) {
            disconnected(closedConnection);
            return;
        }
        failNativeAgreement(NativeAgreement::Failure::Transport);
    }
}
} // namespace gdl::game
