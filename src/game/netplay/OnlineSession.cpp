#include "game/netplay/OnlineSession.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace gdl::game {
namespace {
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
        if (!identity(member.peer) || !peers.insert(member.peer).second || member.seats.empty() ||
            member.seats.size() > 4) {
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
    return peers.contains(room.host) && std::ranges::count(used, true) <= room.settings.maxPlayers;
}
const RoomMember* member(const RoomSnapshot& room, const std::string& peer) {
    const auto found = std::ranges::find(room.members, peer, &RoomMember::peer);
    return found == room.members.end() ? nullptr : &*found;
}
bool sameRoster(const RoomSnapshot& a, const RoomSnapshot& b) {
    return std::ranges::equal(a.members, b.members, [](const auto& x, const auto& y) {
        return x.peer == y.peer && x.seats == y.seats;
    });
}
} // namespace

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

std::vector<std::string> OnlineSession::allowed() const {
    std::vector<std::string> result;
    if (m_room) {
        for (const auto& row : m_room->members) {
            if (row.peer != m_peer && (m_host || row.peer == m_room->host)) {
                result.push_back(row.peer);
            }
        }
    }
    return result;
}
bool OnlineSession::connected() const {
    if (!m_room) {
        return false;
    }
    const auto peers = allowed();
    return std::ranges::all_of(peers, [&](const auto& peer) { return m_links.contains(peer); });
}
void OnlineSession::closeLinks() {
    for (const auto& [peer, connection] : m_links) {
        m_transport.close(connection);
    }
    if (m_connecting) {
        m_transport.close(*m_connecting);
    }
    m_links.clear();
    m_connecting.reset();
    m_prepare.reset();
    m_earlyParty.clear();
}
void OnlineSession::fail(Failure reason) {
    if (m_phase == Phase::Failed || m_phase == Phase::Closed) {
        return;
    }
    m_failure = reason;
    m_phase = Phase::Failed;
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

bool OnlineSession::admit(RoomSnapshot room, const std::string& peer) {
    if (!roster(room) || !identity(peer) || (room.host == peer) != m_host) {
        fail(Failure::Admission);
        return false;
    }
    const auto* local = member(room, peer);
    if (!local || local->seats.size() != m_localPlayers) {
        fail(Failure::Admission);
        return false;
    }
    if (m_room) {
        if (room.code != m_room->code || room.host != m_room->host || peer != m_peer ||
            room.revision < m_room->revision || local->seats != m_seats ||
            (m_room->started && !room.started)) {
            fail(Failure::Admission);
            return false;
        }
        const bool changed = !sameRoster(room, *m_room) || room.settings != m_room->settings;
        if ((changed && room.revision == m_room->revision) ||
            (m_room->started && (changed || room.revision != m_room->revision))) {
            fail(Failure::RosterChanged);
            return false;
        }
        if (room.revision != m_room->revision) {
            m_ready.reset();
            if (!room.started) {
                m_prepare.reset();
            }
            m_wait = 0;
            m_phase = Phase::Lobby;
        }
    } else {
        m_peer = peer;
        m_seats = local->seats;
        if (!m_transport.configurePeer(peer, m_host)) {
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
    if (!m_host && !m_connecting && !m_links.contains(m_room->host)) {
        m_connecting = m_transport.connectPeer(m_room->host);
        if (!m_connecting) {
            fail(Failure::Transport);
            return false;
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
        if (event.connection == 0 || !peer || std::ranges::find(peers, *peer) == peers.end() ||
            m_links.contains(*peer) ||
            std::ranges::any_of(m_links,
                                [&](const auto& row) { return row.second == event.connection; }) ||
            (!m_host && m_connecting != event.connection)) {
            m_transport.close(event.connection);
            return;
        }
        m_links.emplace(*peer, event.connection);
        if (!m_host) {
            m_connecting.reset();
        }
        return;
    }
    const auto link = std::ranges::find_if(
        m_links, [&](const auto& row) { return row.second == event.connection; });
    if (event.type == Type::Disconnected) {
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
            m_match.disconnected(event.connection);
            fail(Failure::Transport);
        } // An already removed lobby member is no longer part of this lifetime.
        return;
    }
    if (link == m_links.end()) {
        return;
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
            if (m_prepare && m_prepare->bytes != event.bytes) {
                fail(Failure::Protocol);
            } else {
                m_prepare = event;
            }
        }
        return;
    }
    const auto admission = m_match.receive(event.connection, event.bytes);
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
    MatchOwners owners{};
    std::vector<MatchLink> links;
    u8 next = 2;
    u8 local = 0;
    for (const auto& row : m_room->members) {
        const u8 peer = row.peer == m_room->host ? 1 : next++;
        for (const auto seat : row.seats) {
            owners[seat] = peer;
        }
        if (row.peer == m_peer) {
            local = peer;
        } else if (const auto link = m_links.find(row.peer); link != m_links.end()) {
            links.push_back({peer, link->second});
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
    if (!m_party.complete()) {
        return;
    }
    if (!m_match.open(local, owners, links)) {
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
        fail(Failure::Service);
        return;
    }
    if (update.room && !admit(std::move(*update.room), update.peer)) {
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
    m_match.flush(m_transport);
    if (m_match.phase() == MatchSession::Phase::Stopped) {
        fail(Failure::Match);
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

bool OnlineSession::select(std::span<const CharacterProfile> selections) {
    if ((m_phase != Phase::Joining && m_phase != Phase::Lobby) ||
        selections.size() != m_localPlayers ||
        !std::ranges::all_of(selections, &CharacterProfile::valid) || m_ready ||
        (m_room && member(*m_room, m_peer)->ready)) {
        return false;
    }
    m_selections.assign(selections.begin(), selections.end());
    return true;
}
bool OnlineSession::ready(bool value) {
    if (m_phase != Phase::Lobby || !m_room || !connected() ||
        (value && m_selections.size() != m_localPlayers)) {
        return false;
    }
    if (m_ready) {
        return m_ready->value == value;
    }
    if (member(*m_room, m_peer)->ready == value) {
        return true;
    }
    if (!m_service.ready(m_room->revision, value)) {
        return false;
    }
    m_ready = ReadyRequest{m_room->revision, value};
    return true;
}
bool OnlineSession::start() {
    if (m_phase != Phase::Lobby || !m_host || !connected() || !m_room || m_ready ||
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
