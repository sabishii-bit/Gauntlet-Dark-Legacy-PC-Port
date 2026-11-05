#include "game/netplay/OnlineSession.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/io/ByteWriter.h"
#include "engine/io/Sha256.h"

#include "game/netplay/RollbackExchange.h"

namespace gdl::game {
namespace {
/** The native contract's rule byte, a bit each. */
constexpr u8 kRuleAutoMelee = 0x1;
constexpr u8 kRuleAutoActivateItems = 0x2;
constexpr u8 kRuleExpandedShop = 0x4;
constexpr u8 kRuleAutoAim = 0x8;
constexpr u8 kRuleMask = kRuleAutoMelee | kRuleAutoActivateItems | kRuleExpandedShop | kRuleAutoAim;

bool identity(const std::string& peer) {
    return peer.size() == 32 && std::ranges::all_of(peer, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool roster(RoomSnapshot& room) {
    if (!room.settings.valid() || room.revision == 0 || room.code.size() != 8 ||
        room.members.empty() || room.members.size() > 4 ||
        !std::ranges::all_of(
            room.code, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9'); })) {
        return false;
    }
    std::set<std::string> peers;
    std::array<bool, 4> used{};
    for (auto& member : room.members) {
        if (!identity(member.endpointId) || !peers.insert(member.endpointId).second ||
            member.seats.empty() || member.seats.size() > 4) {
            return false;
        }
        for (const auto seat : member.seats) {
            if (seat >= used.size() || used[seat]) {
                return false;
            }
            used[seat] = true;
        }
        std::ranges::sort(member.seats);
    }
    // A stable order independent of JSON member ordering and opaque peer IDs.
    std::ranges::sort(room.members, {}, [](const auto& member) { return member.seats.front(); });
    return peers.contains(room.hostEndpointId) &&
           std::ranges::count(used, true) <= room.settings.maxPlayers;
}
const RoomMember* member(const RoomSnapshot& room, const std::string& peer) {
    const auto found = std::ranges::find(room.members, peer, &RoomMember::endpointId);
    return found == room.members.end() ? nullptr : &*found;
}
bool sameRoster(const RoomSnapshot& a, const RoomSnapshot& b) {
    return std::ranges::equal(a.members, b.members, [](const auto& x, const auto& y) {
        return x.endpointId == y.endpointId && x.seats == y.seats;
    });
}
constexpr u32 kNativeMagic = fourcc("GDNB");
constexpr u8 kNativeVersion = 2;
constexpr u8 kNativeInitialization = 1; // canonical NativePlaySetup initialization
constexpr u8 kNativeInputMode = 2;      // native rollback with redundant authenticated inputs
constexpr usize kNativeQueue = 12;      // offer, seal and commit for at most three guests
enum class NativeKind : u8 { Offer, Seal, Acknowledge, Commit };
bool digest(const std::string& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool text(const std::string& value, usize limit) {
    return !value.empty() && value.size() <= limit &&
           std::ranges::all_of(value, [](unsigned char c) { return c >= 32 && c <= 126; });
}
/** A bootstrap string: its length as a u32, then its characters. */
void string(ByteWriter& out, std::string_view value) {
    out.writeU32(static_cast<u32>(value.size()));
    out.writeString(value);
}
std::string string(ByteReader& reader, usize limit) {
    const usize size = reader.readU32();
    if (size == 0 || size > limit) {
        throw FormatError("invalid native bootstrap string length");
    }
    return reader.readString(size);
}
std::string roomDigest(const RoomSnapshot& room) {
    // Canonical admitted roster order, not JSON ordering or local socket IDs.
    // Readiness is intentionally excluded: it changes without a new revision.
    std::vector<u8> bytes;
    ByteWriter out(bytes);
    string(out, "gdl-native-room-v2");
    string(out, room.code);
    string(out, room.hostEndpointId);
    out.writeU64(room.revision);
    out.writeU8(room.settings.maxPlayers);
    out.writeU8(room.settings.difficulty);
    out.writeU8(room.settings.friendlyFire);
    out.writeU8(room.settings.camera);
    out.writeU8(room.settings.mechanics);
    out.writeU8(static_cast<u8>(room.members.size()));
    for (const auto& row : room.members) {
        string(out, row.endpointId);
        out.writeU8(static_cast<u8>(row.seats.size()));
        out.writeBytes(row.seats);
    }
    Sha256 hash;
    hash.update(bytes);
    return hash.hex();
}
struct NativeMessage {
    NativeKind kind;
    std::string scope;
    std::optional<OnlineSession::NativeContract> contract;
    std::string setup;
};
bool nativePacket(std::span<const u8> bytes) {
    return bytes.size() >= 4 && readU32LE(bytes, 0) == kNativeMagic;
}
std::optional<NativeMessage> decodeNative(std::span<const u8> bytes) {
    if (bytes.size() < 72 || bytes.size() > OnlineSession::kNativePacketBytes) {
        return std::nullopt;
    }
    try {
        ByteReader reader(bytes);
        if (reader.readU32() != kNativeMagic || reader.readU8() != kNativeVersion) {
            return std::nullopt;
        }
        const auto kind = reader.readU8();
        if (kind > static_cast<u8>(NativeKind::Commit) || reader.readU8() != kNativeInputMode ||
            reader.readU8() != 0) {
            return std::nullopt;
        }
        NativeMessage result{static_cast<NativeKind>(kind), reader.readString(64), {}, {}};
        if (!digest(result.scope)) {
            return std::nullopt;
        }
        if (result.kind == NativeKind::Offer) {
            OnlineSession::NativeContract contract;
            contract.rules.scene = reader.readU32();
            const u8 flags = reader.readU8();
            contract.rules.startupDelay = reader.readU8();
            if ((flags & ~kRuleMask) != 0 || reader.readU8() != kNativeInitialization ||
                reader.readU8() != 0) {
                return std::nullopt;
            }
            contract.rules.autoMelee = (flags & kRuleAutoMelee) != 0;
            contract.rules.autoActivateItems = (flags & kRuleAutoActivateItems) != 0;
            contract.rules.expandedShop = (flags & kRuleExpandedShop) != 0;
            contract.rules.autoAim = (flags & kRuleAutoAim) != 0;
            contract.identity.build = string(reader, 256);
            contract.identity.runtime = string(reader, 256);
            contract.identity.content = string(reader, 64);
            contract.identity.data = string(reader, 64);
            contract.identity.language = string(reader, 32);
            if (!contract.identity.valid() || !contract.rules.valid()) {
                return std::nullopt;
            }
            result.contract = std::move(contract);
        } else {
            result.setup = reader.readString(64);
            if (!digest(result.setup)) {
                return std::nullopt;
            }
        }
        return reader.atEnd() ? std::optional{std::move(result)} : std::nullopt;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
std::vector<u8> nativeBytes(NativeKind kind, const std::string& scope,
                            const OnlineSession::NativeContract& contract,
                            const std::string& setup = {}) {
    std::vector<u8> bytes;
    ByteWriter out(bytes);
    out.writeU32(kNativeMagic);
    out.writeU8(kNativeVersion);
    out.writeU8(static_cast<u8>(kind));
    out.writeU8(kNativeInputMode);
    out.writeU8(0);
    out.writeString(scope);
    if (kind == NativeKind::Offer) {
        const auto& rules = contract.rules;
        out.writeU32(rules.scene);
        const auto flags = static_cast<u8>((rules.autoMelee ? kRuleAutoMelee : 0U) |
                                           (rules.autoActivateItems ? kRuleAutoActivateItems : 0U) |
                                           (rules.expandedShop ? kRuleExpandedShop : 0U) |
                                           (rules.autoAim ? kRuleAutoAim : 0U));
        out.writeU8(flags);
        out.writeU8(rules.startupDelay);
        out.writeU8(kNativeInitialization);
        out.writeU8(0);
        string(out, contract.identity.build);
        string(out, contract.identity.runtime);
        string(out, contract.identity.content);
        string(out, contract.identity.data);
        string(out, contract.identity.language);
    } else {
        out.writeString(setup);
    }
    return bytes;
}
} // namespace

struct OnlineSession::NativeBootstrap {
    NativeIdentity local;
    std::optional<NativeRules> hostRules;
    std::optional<NativeContract> contract;
    std::string scope;
    std::string localSetup;
    std::string sealedSetup;
    std::set<Connection> acknowledged;
    bool acknowledgmentQueued = false;
    bool committed = false;
    std::vector<PacketTransport::Event> early;
    struct Delivery {
        Connection connection;
        std::vector<u8> bytes;
    };
    std::deque<Delivery> outgoing;
};

bool OnlineSession::NativeIdentity::valid() const {
    return text(build, 256) && text(runtime, 256) && digest(content) && digest(data) &&
           text(language, 32);
}
bool OnlineSession::NativeRules::valid() const {
    return scene > 0 && scene <= 65535 && scene % 256 != 0 &&
           startupDelay <= RollbackInputs::kMaxDelay;
}

OnlineSession::OnlineSession(RoomService& service, PeerTransport& transport, bool host,
                             u8 localPlayers)
    : m_service(service), m_transport(transport), m_host(host), m_localPlayers(localPlayers) {
    if (localPlayers == 0 || localPlayers > 4) {
        fail(Failure::Admission);
    }
}
OnlineSession::~OnlineSession() {
    leave();
}

bool OnlineSession::configureNative(const NativeIdentity& local,
                                    std::optional<NativeRules> hostRules) {
    if ((m_phase != Phase::Joining && m_phase != Phase::Lobby) || m_native || m_ready ||
        (m_room && member(*m_room, m_endpointId)->ready) || !local.valid() ||
        m_host != hostRules.has_value() || (hostRules && !hostRules->valid())) {
        return false;
    }
    m_native = std::make_unique<NativeBootstrap>();
    m_native->local = local;
    m_native->hostRules = hostRules;
    // Compatibility identifiers only: never log room codes, endpoint IDs or profile data.
    log::info("Native online identity: role={} build={} runtime={} content={} data={} language={}",
              m_host ? "host" : "guest", local.build, local.runtime, local.content, local.data,
              local.language);
    return true;
}
const OnlineSession::NativeContract* OnlineSession::nativeContract() const {
    return m_native && m_native->contract ? &*m_native->contract : nullptr;
}
bool OnlineSession::nativeSetupPending() const {
    return m_phase == Phase::Starting && m_party.complete() && nativeContract() != nullptr &&
           m_native->localSetup.empty();
}
std::string_view OnlineSession::confirmedNativeSetup() const {
    return m_native && m_native->committed && m_phase == Phase::Active
               ? std::string_view{m_native->localSetup}
               : std::string_view{};
}
bool OnlineSession::confirmNativeSetup(const std::string& setup) {
    if (m_phase != Phase::Starting || !m_party.complete() || !nativeContract() || !digest(setup)) {
        return false;
    }
    if ((!m_native->localSetup.empty() && m_native->localSetup != setup) ||
        (!m_native->sealedSetup.empty() && m_native->sealedSetup != setup)) {
        m_failureDetail = "Native simulation initial setup differs";
        fail(Failure::Protocol);
        return false;
    }
    m_native->localSetup = setup;
    return true;
}

bool OnlineSession::nativeMessage(Connection connection, std::span<const u8> bytes) {
    const auto message = decodeNative(bytes);
    if (!m_native || !message) {
        m_failureDetail = "Native simulation mode or bootstrap packet differs";
        return false;
    }
    const auto link = std::ranges::find_if(
        m_links, [&](const auto& value) { return value.second == connection; });
    if (link == m_links.end() || (!m_host && link->first != m_room->hostEndpointId) ||
        (m_host && message->kind != NativeKind::Acknowledge) ||
        (!m_host && message->kind == NativeKind::Acknowledge)) {
        return false;
    }
    if (m_native->scope.empty()) {
        if (m_host || (message->kind != NativeKind::Offer && message->kind != NativeKind::Seal)) {
            return false;
        }
        for (const auto& pending : m_native->early) {
            if (pending.bytes[5] == bytes[5]) {
                return std::ranges::equal(pending.bytes, bytes);
            }
        }
        if (m_native->early.size() >= 2) {
            return false;
        }
        m_native->early.push_back(
            {PacketTransport::EventType::Message, connection, {bytes.begin(), bytes.end()}, {}});
        return true;
    }
    if (message->scope != m_native->scope) {
        m_failureDetail = "Native simulation room scope differs";
        return false;
    }
    if (message->kind == NativeKind::Offer) {
        if (!message->contract || message->contract->identity != m_native->local) {
            if (message->contract) {
                const auto& remote = message->contract->identity;
                const auto& local = m_native->local;
                log::warn("Native compatibility rejected: local_build={} host_build={} "
                          "build_equal={} runtime_equal={} content_equal={} data_equal={} "
                          "language_equal={}",
                          local.build, remote.build, local.build == remote.build,
                          local.runtime == remote.runtime, local.content == remote.content,
                          local.data == remote.data, local.language == remote.language);
            }
            m_failureDetail = "Native simulation executable, runtime, content or language differs";
            return false;
        }
        if (m_native->contract) {
            return *m_native->contract == *message->contract;
        }
        m_native->contract = message->contract;
        return true;
    }
    if (!m_native->contract) {
        return false;
    }
    if (message->kind == NativeKind::Seal) {
        if ((!m_native->sealedSetup.empty() && m_native->sealedSetup != message->setup) ||
            (!m_native->localSetup.empty() && m_native->localSetup != message->setup)) {
            m_failureDetail = "Native simulation initial setup differs";
            return false;
        }
        m_native->sealedSetup = message->setup;
        return true;
    }
    if (m_native->sealedSetup != message->setup || m_native->localSetup != message->setup) {
        return false;
    }
    if (message->kind == NativeKind::Acknowledge) {
        m_native->acknowledged.insert(connection);
        return true;
    }
    if (!m_party.complete() || !m_native->acknowledgmentQueued) {
        return false;
    }
    m_native->committed = true;
    return true;
}

bool OnlineSession::advanceNativeBootstrap() {
    if (!m_native) {
        return false;
    }
    const auto queue = [&](NativeKind kind, bool broadcast) {
        const auto count = broadcast ? m_links.size() : usize{1};
        if (!m_native->contract || m_native->outgoing.size() + count > kNativeQueue) {
            return false;
        }
        const auto bytes =
            nativeBytes(kind, m_native->scope, *m_native->contract, m_native->localSetup);
        for (const auto& [peer, connection] : m_links) {
            if (broadcast || peer == m_room->hostEndpointId) {
                m_native->outgoing.push_back({connection, bytes});
            }
        }
        return true;
    };
    if (m_native->scope.empty()) {
        m_native->scope = roomDigest(*m_room);
        if (m_host) {
            m_native->contract = NativeContract{m_native->local, *m_native->hostRules};
            if (!queue(NativeKind::Offer, true)) {
                return false;
            }
        }
        auto early = std::move(m_native->early);
        m_native->early.clear();
        for (const auto& packet : early) {
            if (!nativeMessage(packet.connection, packet.bytes)) {
                return false;
            }
        }
    }
    if (!m_party.complete() || !m_native->contract || m_native->localSetup.empty()) {
        return true;
    }
    if (m_host) {
        if (m_native->sealedSetup.empty()) {
            if (!queue(NativeKind::Seal, true)) {
                return false;
            }
            m_native->sealedSetup = m_native->localSetup;
        }
        if (!m_native->committed && m_native->acknowledged.size() == m_links.size()) {
            if (!queue(NativeKind::Commit, true)) {
                return false;
            }
            m_native->committed = true;
        }
    } else if (!m_native->sealedSetup.empty() && !m_native->acknowledgmentQueued) {
        if (m_native->sealedSetup != m_native->localSetup ||
            !queue(NativeKind::Acknowledge, false)) {
            return false;
        }
        m_native->acknowledgmentQueued = true;
    }
    return true;
}
bool OnlineSession::flushNativeBootstrap() {
    if (!m_native) {
        return true;
    }
    while (!m_native->outgoing.empty()) {
        const auto& next = m_native->outgoing.front();
        const auto result =
            m_transport.send(next.connection, next.bytes, PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return true;
        }
        if (result != PacketTransport::SendResult::Sent) {
            return false;
        }
        m_native->outgoing.pop_front();
    }
    return true;
}

bool OnlineSession::departed(const RoomSnapshot& next) {
    if (!next.started || next.settings != m_room->settings ||
        next.members.size() >= m_room->members.size()) {
        return false;
    }
    const bool unchanged = std::ranges::all_of(next.members, [&](const RoomMember& row) {
        const auto* before = member(*m_room, row.endpointId);
        return before != nullptr && before->seats == row.seats;
    });
    if (!unchanged) {
        return false;
    }
    // Every member gone is handled as its lost link would be.
    return std::ranges::all_of(m_room->members, [&](const RoomMember& row) {
        if (member(next, row.endpointId) != nullptr) {
            return true;
        }
        if (row.endpointId == m_endpointId || row.endpointId == m_room->hostEndpointId) {
            return false;
        }
        const auto link = m_links.find(row.endpointId);
        if (link == m_links.end()) {
            return true; // its link closed first and was handled then
        }
        if (m_host) {
            if (!m_match.disconnected(link->second)) {
                return false;
            }
        } else {
            m_match.directLost(link->second);
        }
        m_links.erase(link);
        return true;
    });
}
std::vector<std::string> OnlineSession::allowed() const {
    std::vector<std::string> result;
    if (m_room) {
        for (const auto& row : m_room->members) {
            if (row.endpointId != m_endpointId) {
                result.push_back(row.endpointId);
            }
        }
    }
    return result;
}
std::vector<std::string> OnlineSession::required() const {
    std::vector<std::string> result;
    if (m_room) {
        for (const auto& row : m_room->members) {
            if (row.endpointId != m_endpointId &&
                (m_host || row.endpointId == m_room->hostEndpointId)) {
                result.push_back(row.endpointId);
            }
        }
    }
    return result;
}
usize OnlineSession::directLinks() const {
    if (m_host || !m_room) {
        return 0;
    }
    return static_cast<usize>(std::ranges::count_if(
        m_links, [&](const auto& row) { return row.first != m_room->hostEndpointId; }));
}
std::optional<PacketTransport::Statistics> OnlineSession::linkStatistics() const {
    std::optional<PacketTransport::Statistics> worst;
    for (const auto& [peer, connection] : m_links) {
        const auto link = m_transport.statistics(connection);
        if (!link) {
            continue;
        }
        if (!worst) {
            worst = *link;
            continue;
        }
        worst->pingMs = std::max(worst->pingMs, link->pingMs);
        worst->pendingBytes += link->pendingBytes;
        worst->queueMicroseconds = std::max(worst->queueMicroseconds, link->queueMicroseconds);
        worst->relayed = worst->relayed || link->relayed;
    }
    return worst;
}

bool OnlineSession::connected() const {
    if (!m_room) {
        return false;
    }
    const auto peers = required();
    return std::ranges::all_of(peers, [&](const auto& peer) { return m_links.contains(peer); });
}
void OnlineSession::closeLinks() {
    for (const auto& [peer, connection] : m_links) {
        m_transport.close(connection);
    }
    for (const auto& [peer, connection] : m_dialing) {
        m_transport.close(connection);
    }
    if (m_connecting) {
        m_transport.close(*m_connecting);
    }
    m_links.clear();
    m_dialing.clear();
    m_connecting.reset();
    m_prepare.reset();
    m_earlyParty.clear();
    if (m_native) {
        m_native->early.clear();
        m_native->outgoing.clear();
    }
}
void OnlineSession::fail(Failure reason) {
    if (m_phase == Phase::Failed || m_phase == Phase::Closed) {
        return;
    }
    m_failure = reason;
    m_phase = Phase::Failed;
    if (m_failureDetail.empty()) {
        // Keep the first local cause before leave() tears down callbacks/history.
        // Match diagnostics contain no peer-controlled text or private room code.
        m_failureDetail = m_match.failureDetail();
    }
    if (m_failureDetail.empty()) {
        switch (reason) {
        case Failure::None: break;
        case Failure::Admission: m_failureDetail = "The room admission was rejected."; break;
        case Failure::Service: m_failureDetail = "The room service connection ended."; break;
        case Failure::Transport:
            m_failureDetail = "The connection to another player failed or was lost.";
            break;
        case Failure::Protocol:
            m_failureDetail = "Received invalid or incompatible online session data.";
            break;
        case Failure::RosterChanged:
            m_failureDetail = "The room roster changed after the game started.";
            break;
        case Failure::Timeout:
            m_failureDetail = "Timed out while joining or starting the online game.";
            break;
        case Failure::Match: m_failureDetail = "The online game stopped."; break;
        }
    }
    log::warn("Online session failed: {}", m_failureDetail);
    m_match.leave();
    m_match.flush(m_transport);
    closeLinks();
    m_service.leave();
}
void OnlineSession::leave() {
    if (m_phase == Phase::Leaving || m_phase == Phase::Closed || m_phase == Phase::Failed) {
        return;
    }
    m_match.leave();
    m_match.flush(m_transport);
    closeLinks();
    m_service.leave();
    m_phase = Phase::Leaving;
    m_wait = 0;
}

bool OnlineSession::admit(RoomSnapshot room, const std::string& endpointId) {
    if (!roster(room) || !identity(endpointId) || (room.hostEndpointId == endpointId) != m_host) {
        fail(Failure::Admission);
        return false;
    }
    const auto* local = member(room, endpointId);
    if (!local || local->seats.size() != m_localPlayers) {
        fail(Failure::Admission);
        return false;
    }
    if (m_room) {
        if (room.code != m_room->code || room.hostEndpointId != m_room->hostEndpointId ||
            endpointId != m_endpointId || room.revision < m_room->revision ||
            local->seats != m_seats || (m_room->started && !room.started)) {
            fail(Failure::Admission);
            return false;
        }
        const bool changed = !sameRoster(room, *m_room) || room.settings != m_room->settings;
        if ((changed && room.revision == m_room->revision) ||
            (m_room->started && (changed || room.revision != m_room->revision))) {
            // The room may report a member's departure before its link closes, or after.
            if (!m_room->started || !departed(room)) {
                fail(Failure::RosterChanged);
                return false;
            }
        } else if (room.revision != m_room->revision) {
            m_ready.reset();
            if (!room.started) {
                m_prepare.reset();
            }
            m_wait = 0;
            m_phase = Phase::Lobby;
        }
    } else {
        m_endpointId = endpointId;
        m_seats = local->seats;
        if (!m_transport.configurePeer(endpointId, m_host)) {
            fail(Failure::Transport);
            return false;
        }
        m_phase = Phase::Lobby;
        m_wait = 0;
    }
    if (m_ready && m_ready->revision == room.revision && m_ready->value == local->ready) {
        m_ready.reset();
    }
    if (m_phase == Phase::Starting && !room.started &&
        !std::ranges::all_of(room.members, [](const auto& row) { return row.ready; })) {
        // Ready/unready does not change membership revision. A guest can unready
        // just before the host's start request reaches the room authority.
        m_phase = Phase::Lobby;
        m_wait = 0;
    }
    const bool starting = room.started && (!m_room || !m_room->started);
    m_room = std::move(room);
    const auto peers = allowed();
    // Remove state before the transport emits disconnect events for departed peers.
    std::erase_if(m_links, [&](const auto& row) {
        return std::ranges::find(peers, row.first) == peers.end();
    });
    if (!m_transport.authorizePeers(peers)) {
        fail(Failure::Transport);
        return false;
    }
    if (!m_host && !m_connecting && !m_links.contains(m_room->hostEndpointId)) {
        m_connecting = m_transport.connectPeer(m_room->hostEndpointId);
        if (!m_connecting) {
            fail(Failure::Transport);
            return false;
        }
    }
    if (!m_host) {
        // Direct links are a bonus: each pair links once, the member with the greater
        // id dialling, and a dial that fails leaves that peer on the host's relay.
        std::erase_if(m_dialing, [&](const auto& row) {
            return std::ranges::find(peers, row.first) == peers.end();
        });
        for (const auto& row : m_room->members) {
            if (row.endpointId == m_endpointId || row.endpointId == m_room->hostEndpointId ||
                row.endpointId < m_endpointId || m_links.contains(row.endpointId) ||
                m_dialing.contains(row.endpointId)) {
                continue;
            }
            if (const auto id = m_transport.connectPeer(row.endpointId)) {
                m_dialing.emplace(row.endpointId, *id);
            }
        }
    }
    if (starting) {
        m_phase = Phase::Starting;
        m_wait = 0;
    }
    return true;
}

void OnlineSession::event(const PacketTransport::Event& event) {
    using Type = PacketTransport::EventType;
    if (event.type == Type::Connected) {
        const auto peer = m_transport.peer(event.connection);
        const auto peers = allowed();
        if (peer && m_links.contains(*peer) && m_links.at(*peer) == event.connection) {
            return;
        }
        const bool dialed =
            peer && m_dialing.contains(*peer) && m_dialing.at(*peer) == event.connection;
        const bool accepted =
            peer && !m_host && m_room && *peer != m_room->hostEndpointId && *peer < m_endpointId;
        if (event.connection == 0 || !peer || std::ranges::find(peers, *peer) == peers.end() ||
            m_links.contains(*peer) ||
            std::ranges::any_of(m_links,
                                [&](const auto& row) { return row.second == event.connection; }) ||
            (!m_host && m_connecting != event.connection && !dialed && !accepted)) {
            m_transport.close(event.connection);
            return;
        }
        m_links.emplace(*peer, event.connection);
        if (dialed) {
            m_dialing.erase(*peer);
        }
        if (!m_host && m_connecting == event.connection) {
            m_connecting.reset();
        }
        if (!m_host && m_room && *peer != m_room->hostEndpointId) {
            log::info("Online direct link with another guest is up");
        }
        return;
    }
    const auto link = std::ranges::find_if(
        m_links, [&](const auto& row) { return row.second == event.connection; });
    const bool direct =
        link != m_links.end() && !m_host && m_room && link->first != m_room->hostEndpointId;
    if (event.type == Type::Disconnected) {
        if (const auto dial = std::ranges::find_if(
                m_dialing, [&](const auto& row) { return row.second == event.connection; });
            dial != m_dialing.end()) {
            m_dialing.erase(dial); // the dial failed; the host relays that peer
            return;
        }
        if (direct) {
            m_match.directLost(event.connection);
            m_links.erase(link);
            return;
        }
        if (link != m_links.end() || m_connecting == event.connection) {
            if (m_host && m_phase == Phase::Lobby && link != m_links.end()) {
                m_links.erase(link);
                m_ready.reset();
                m_wait = 0;
                // A guest may leave before its room-service update reaches us.
                // Keep the lobby alive, but it cannot start with a missing link.
                if (m_service.ready(m_room->revision, false)) {
                    m_ready = ReadyRequest{m_room->revision, false};
                }
                return;
            }
            if (m_match.disconnected(event.connection)) {
                // The match dropped that guest and carries on without it.
                if (link != m_links.end()) {
                    m_links.erase(link);
                }
                return;
            }
            fail(Failure::Transport);
        } // An already removed lobby member is no longer part of this lifetime.
        return;
    }
    if (link == m_links.end()) {
        return;
    }
    if (direct) {
        // A direct link carries the other guest's inputs and nothing else; anything
        // it gets wrong is left to the host's relay rather than failing the session.
        if (m_match.phase() != MatchSession::Phase::Offline &&
            RollbackExchange::recognizes(event.bytes)) {
            const auto statistics = m_transport.statistics(event.connection);
            m_match.receive(event.connection, event.bytes, std::nullopt,
                            statistics ? statistics->pingMs : -1);
        }
        return;
    }
    if (nativePacket(event.bytes)) {
        if (!nativeMessage(event.connection, event.bytes)) {
            fail(Failure::Protocol);
        }
        return;
    }
    {
        const auto control = MatchControlPacket::decode(event.bytes);
        if (control && control->kind == MatchControlKind::Prepare &&
            control->context.transition == MatchTransition::Start &&
            (!m_native || !m_native->committed || !m_native->contract ||
             control->context.scene != m_native->contract->rules.scene ||
             control->context.inputLead != m_native->contract->rules.startupDelay)) {
            m_failureDetail = "Native simulation start differs from the agreed contract";
            fail(Failure::Protocol);
            return;
        }
    }
    if (PartyBootstrap::recognizes(event.bytes)) {
        if (!m_party.opened()) {
            // The peer's reliable party can overtake our room start update.
            // One profile per seat plus its seal is the entire bounded exchange.
            if (m_earlyParty.size() >= InputCommand::kSeats + 1 ||
                !PartyBootstrap::validPacket(event.bytes)) {
                fail(Failure::Protocol);
            } else {
                m_earlyParty.push_back(event);
            }
        } else if (!m_party.receive(event.connection, event.bytes)) {
            fail(Failure::Protocol);
        }
        return;
    }
    if (m_match.phase() == MatchSession::Phase::Offline) {
        // Reliable Prepare can outrun the guest's room start update. Retain
        // exactly one validated control, never arbitrary early gameplay packets.
        const auto control = MatchControlPacket::decode(event.bytes);
        if (!m_host && control && control->kind == MatchControlKind::Prepare &&
            control->context.transition == MatchTransition::Start) {
            if (!m_native || !m_native->committed ||
                (m_prepare && m_prepare->bytes != event.bytes)) {
                fail(Failure::Protocol);
            } else {
                m_prepare = event;
            }
        }
        return;
    }
    const auto statistics = m_transport.statistics(event.connection);
    const auto admission = m_match.receive(event.connection, event.bytes, std::nullopt,
                                           statistics ? statistics->pingMs : -1);
    if (admission != MatchSession::Admission::Accepted &&
        admission != MatchSession::Admission::Stale) {
        fail(Failure::Protocol);
    } else if (m_match.phase() == MatchSession::Phase::Stopped) {
        // Ignore the rest of this receive batch after authoritative termination.
        fail(Failure::Match);
    }
}

void OnlineSession::openMatch() {
    if (!m_room || !m_room->started || !connected() ||
        m_match.phase() != MatchSession::Phase::Offline) {
        return;
    }
    if (!m_native) {
        m_failureDetail = "Native simulation identity was not configured before room start";
        fail(Failure::Protocol);
        return;
    }
    MatchOwners owners{};
    std::vector<MatchLink> links;
    std::vector<MatchLink> direct;
    u8 next = 2;
    u8 local = 0;
    for (const auto& row : m_room->members) {
        const u8 peer = row.endpointId == m_room->hostEndpointId ? 1 : next++;
        for (const auto seat : row.seats) {
            owners[seat] = peer;
        }
        if (row.endpointId == m_endpointId) {
            local = peer;
        } else if (const auto link = m_links.find(row.endpointId); link != m_links.end()) {
            (m_host || row.endpointId == m_room->hostEndpointId ? links : direct)
                .push_back({peer, link->second});
        }
    }
    if (!m_party.opened()) {
        if (!m_party.open(local, owners, links, m_selections)) {
            fail(Failure::Protocol);
            return;
        }
        auto early = std::move(m_earlyParty);
        m_earlyParty.clear();
        for (const auto& packet : early) {
            if (!m_party.receive(packet.connection, packet.bytes)) {
                fail(Failure::Protocol);
                return;
            }
        }
    }
    if (!advanceNativeBootstrap()) {
        fail(Failure::Protocol);
        return;
    }
    if (!m_party.complete() || !m_native->committed) {
        return;
    }
    if (!m_match.open(local, owners, links, m_native->contract->rules.startupDelay, direct)) {
        fail(Failure::Protocol);
        return;
    }
    m_phase = Phase::Active;
    m_wait = 0;
    if (m_prepare) {
        auto pending = std::move(*m_prepare);
        m_prepare.reset();
        event(pending);
    }
}

void OnlineSession::update(f64 seconds) {
    if (m_phase == Phase::Failed || m_phase == Phase::Closed) {
        return;
    }
    if (!std::isfinite(seconds) || seconds < 0) {
        fail(Failure::Protocol);
        return;
    }
    m_wait += seconds;
    auto update = m_service.poll();
    if (m_phase == Phase::Leaving) {
        if (update.closed || m_wait >= kLeaveTimeout) {
            m_phase = Phase::Closed;
        }
        return;
    }
    if (update.closed || !update.error.empty()) {
        m_failureDetail = update.error.empty() ? "Room closed" : update.error;
        log::warn("Online room failed: {}", m_failureDetail);
        fail(Failure::Service);
        return;
    }
    if (update.room && !admit(std::move(*update.room), update.endpointId)) {
        return;
    }
    for (const auto& item : m_transport.poll()) {
        event(item);
        if (m_phase == Phase::Failed) {
            return;
        }
    }
    openMatch();
    if (m_phase == Phase::Failed) {
        return;
    }
    if (!m_party.flush(m_transport)) {
        fail(Failure::Transport);
        return;
    }
    m_match.update(seconds);
    if (!flush()) {
        return;
    }
    if (m_phase == Phase::Lobby && connected()) {
        m_wait = 0;
    }
    if ((m_phase == Phase::Joining && m_wait >= kAdmissionTimeout) ||
        (m_phase == Phase::Lobby && m_wait >= kConnectionTimeout) ||
        (m_phase == Phase::Starting && m_wait >= kStartTimeout)) {
        fail(Failure::Timeout);
    }
}

bool OnlineSession::flush() {
    if (m_phase == Phase::Failed || m_phase == Phase::Closed) {
        return false;
    }
    if (!flushNativeBootstrap()) {
        fail(Failure::Transport);
        return false;
    }
    if (m_native && !m_native->outgoing.empty()) {
        return true; // Commit must precede any native Prepare on the same reliable link.
    }
    if (m_phase == Phase::Active) {
        if (m_native && m_match.context().epoch != 0 &&
            m_match.context().transition == MatchTransition::Start &&
            (m_match.context().scene != m_native->contract->rules.scene ||
             m_match.context().inputLead != m_native->contract->rules.startupDelay)) {
            m_failureDetail = "Native simulation start differs from the agreed contract";
            fail(Failure::Protocol);
            return false;
        }
        m_match.flush(m_transport);
        if (m_match.phase() == MatchSession::Phase::Stopped) {
            fail(Failure::Match);
            return false;
        }
    }
    return true;
}

bool OnlineSession::select(std::span<const CharacterProfile> selections) {
    if ((m_phase != Phase::Joining && m_phase != Phase::Lobby) ||
        selections.size() != m_localPlayers ||
        !std::ranges::all_of(selections, &CharacterProfile::valid) || m_ready ||
        (m_room && member(*m_room, m_endpointId)->ready)) {
        return false;
    }
    m_selections.assign(selections.begin(), selections.end());
    return true;
}
bool OnlineSession::ready(bool value) {
    if (m_phase != Phase::Lobby || !m_room || !connected() ||
        (value && (!m_native || m_selections.size() != m_localPlayers))) {
        return false;
    }
    if (m_ready) {
        return m_ready->value == value;
    }
    if (member(*m_room, m_endpointId)->ready == value) {
        return true;
    }
    if (!m_service.ready(m_room->revision, value)) {
        return false;
    }
    m_ready = ReadyRequest{m_room->revision, value};
    return true;
}
bool OnlineSession::adoptStartupDelay(u8 delay) {
    if (!m_host || !m_native || !m_native->hostRules || m_native->contract ||
        delay > RollbackInputs::kMaxDelay ||
        (m_phase != Phase::Lobby && m_phase != Phase::Joining)) {
        return false;
    }
    m_native->hostRules->startupDelay = delay;
    return true;
}
bool OnlineSession::start() {
    if (m_phase != Phase::Lobby || !m_host || !connected() || !m_room || !m_native || m_ready ||
        m_selections.size() != m_localPlayers ||
        !std::ranges::all_of(m_room->members, [](const auto& row) { return row.ready; }) ||
        !m_service.start(m_room->revision)) {
        return false;
    }
    m_phase = Phase::Starting;
    m_wait = 0;
    return true;
}
} // namespace gdl::game
