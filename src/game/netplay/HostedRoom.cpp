#include "game/netplay/HostedRoom.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
// Match checkpoint capabilities at admission, including development builds that
// share a semver but cannot safely consume one another's replay envelopes.
constexpr u8 kProtocol = 38;
constexpr usize kPending = 16;
constexpr usize kCardLimit = 1024;
enum class Kind : u8 { Game, Join, Roster, Ready, Reject, Card, Hello };
std::vector<u8> packet(Kind kind) {
    return {static_cast<u8>(kind), kProtocol};
}
void number(std::vector<u8>& out, u64 value) {
    for (u32 shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<u8>(value >> shift));
    }
}
u64 number(ByteReader& in) {
    const u64 low = in.readU32();
    return low | (u64{in.readU32()} << 32);
}
void string(std::vector<u8>& out, const std::string& value) {
    out.push_back(static_cast<u8>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
std::string string(ByteReader& in) {
    const auto bytes = in.readBytes(in.readU8());
    return {bytes.begin(), bytes.end()};
}
/** A card, up to kCardLimit bytes; longer ones travel as empty (no direct link). */
void card(std::vector<u8>& out, const std::string& value) {
    const auto size = value.size() <= kCardLimit ? value.size() : 0;
    out.push_back(static_cast<u8>(size & 0xffU));
    out.push_back(static_cast<u8>(size >> 8U));
    out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(size));
}
std::string card(ByteReader& in) {
    const usize low = in.readU8();
    const usize high = in.readU8();
    const auto bytes = in.readBytes(low | (high << 8U));
    return {bytes.begin(), bytes.end()};
}
std::string peerId(u64 id) {
    return std::format("{:032x}", id);
}
bool contentHash(const std::string& text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
} // namespace

HostedRoom::HostedRoom(PacketTransport& wire, bool host, u8 players, std::string build,
                       std::string content)
    : m_wire(wire), m_host(host), m_players(players), m_build(std::move(build)),
      m_content(std::move(content)), m_transport(*this) {
    if (players == 0 || players > 4 || m_build.empty() || m_build.size() > 128 ||
        !contentHash(m_content)) {
        fail(Failure::Protocol);
        return;
    }
    if (host) {
        m_peer = peerId(1);
        RoomMember member{m_peer, {}, false};
        for (u8 seat = 0; seat < players; ++seat) {
            member.seats.push_back(seat);
        }
        m_room = {"HOSTROOM", m_peer, 1, false, {std::move(member)}};
        m_dirty = true;
    }
}
HostedRoom::~HostedRoom() {
    leave();
}
void HostedRoom::fail(Failure failure) {
    if (m_failure == Failure::None) {
        m_failure = failure;
    }
}
bool HostedRoom::queue(Connection id, std::vector<u8> bytes) {
    const auto found = m_links.find(id);
    if (found == m_links.end() || found->second.pending.size() >= kPending ||
        bytes.size() > PacketTransport::kMaxPacketBytes) {
        return false;
    }
    found->second.pending.push_back(std::move(bytes));
    return true;
}
void HostedRoom::changed(bool membership) {
    if (membership) {
        ++m_room.revision;
        for (auto& member : m_room.members) {
            member.ready = false;
        }
    }
    m_dirty = true;
    for (const auto& [id, link] : m_links) {
        if (link.peer.empty() || link.rejected) {
            continue;
        }
        auto bytes = packet(Kind::Roster);
        string(bytes, link.peer);
        number(bytes, m_room.revision);
        bytes.push_back(m_room.started ? 1 : 0);
        bytes.push_back(m_room.settings.maxPlayers);
        bytes.push_back(m_room.settings.difficulty);
        bytes.push_back(m_room.settings.friendlyFire);
        bytes.push_back(m_room.settings.camera);
        bytes.push_back(m_room.settings.mechanics);
        bytes.push_back(static_cast<u8>(m_room.members.size()));
        for (const auto& member : m_room.members) {
            string(bytes, member.peer);
            bytes.push_back(member.ready ? 1 : 0);
            bytes.push_back(static_cast<u8>(member.seats.size()));
            bytes.insert(bytes.end(), member.seats.begin(), member.seats.end());
        }
        if (!queue(id, std::move(bytes))) {
            fail(Failure::Connection);
        }
        // Every other member's card follows, so guests can dial each other directly.
        for (const auto& member : m_room.members) {
            const auto where = member.peer == m_peer ? m_wire.address() : m_cards[member.peer];
            if (member.peer == link.peer || where.empty()) {
                continue;
            }
            auto bytes = packet(Kind::Card);
            string(bytes, member.peer);
            card(bytes, where);
            if (!queue(id, std::move(bytes))) {
                fail(Failure::Connection);
            }
        }
    }
}
void HostedRoom::announce(Connection id) {
    const auto found = m_links.find(id);
    if (found == m_links.end()) {
        return;
    }
    auto& link = found->second;
    if (link.announced || link.rejected || link.peer.empty() || !m_transport.allowed(link.peer) ||
        (link.direct && !link.hello)) {
        return;
    }
    link.announced = true;
    m_events.push_back({PacketTransport::EventType::Connected, id, {}, {}});
}
void HostedRoom::disconnect(Connection id) {
    const auto found = m_links.find(id);
    if (found == m_links.end()) {
        return;
    }
    const auto peer = found->second.peer;
    // A direct link reports its end even before it was announced: the dial failed.
    if (found->second.announced || found->second.direct) {
        m_events.push_back({PacketTransport::EventType::Disconnected, id, {}, {}});
    }
    m_wire.close(id);
    m_links.erase(found);
    if (!m_host) {
        if (id == m_hostLink) {
            fail(Failure::Connection);
        }
    } else if (!peer.empty()) {
        // A started room keeps its roster; the match decides what a lost guest means.
        if (!m_room.started) {
            std::erase_if(m_room.members, [&](const auto& row) { return row.peer == peer; });
            changed(true);
        }
    }
}
void HostedRoom::message(Connection id, std::span<const u8> bytes) {
    auto& link = m_links.at(id);
    if (link.rejected || bytes.empty()) {
        return;
    }
    if (bytes[0] == static_cast<u8>(Kind::Game)) {
        if (!link.announced || bytes.size() <= 1 ||
            bytes.size() > PacketTransport::kMaxPacketBytes + 1) {
            disconnect(id);
            return;
        }
        m_events.push_back(
            {PacketTransport::EventType::Message, id, {bytes.begin() + 1, bytes.end()}, {}});
        return;
    }
    try {
        ByteReader reader(bytes);
        const auto kind = static_cast<Kind>(reader.readU8());
        if (reader.readU8() != kProtocol) {
            disconnect(id);
            return;
        }
        if (kind == Kind::Join && m_host && link.peer.empty()) {
            const auto players = reader.readU8();
            const auto build = string(reader);
            const auto content = string(reader);
            const auto where = card(reader);
            if (!reader.atEnd() || players == 0 || players > 4) {
                disconnect(id);
                return;
            }
            std::array<bool, 4> occupied{};
            for (const auto& member : m_room.members) {
                for (const auto seat : member.seats) {
                    occupied[seat] = true;
                }
            }
            const auto free = m_room.settings.maxPlayers - std::ranges::count(occupied, true);
            Failure failure = Failure::None;
            if (m_room.started) {
                failure = Failure::Started;
            } else if (build != m_build) {
                failure = Failure::Version;
            } else if (content != m_content) {
                failure = Failure::Assets;
            } else if (players > free) {
                failure = Failure::Full;
            }
            if (failure != Failure::None) {
                auto rejection = packet(Kind::Reject);
                rejection.push_back(static_cast<u8>(failure));
                queue(id, std::move(rejection));
                link.rejected = true;
                return;
            }
            link.peer = peerId(id + 1);
            if (!where.empty()) {
                m_cards[link.peer] = where;
            }
            RoomMember member{link.peer, {}, false};
            for (u8 seat = 0; seat < 4 && member.seats.size() < players; ++seat) {
                if (!occupied[seat]) {
                    member.seats.push_back(seat);
                }
            }
            m_room.members.push_back(std::move(member));
            changed(true);
        } else if (kind == Kind::Roster && !m_host) {
            const auto peer = string(reader);
            RoomSnapshot room{"HOSTROOM", peerId(1), number(reader), false, {}};
            const auto started = reader.readU8();
            room.settings = {reader.readU8(), reader.readU8(), reader.readU8(), reader.readU8(),
                             reader.readU8()};
            const auto count = reader.readU8();
            if (started > 1 || count == 0 || count > 4 || !room.settings.valid()) {
                disconnect(id);
                return;
            }
            room.started = started != 0;
            for (u8 i = 0; i < count; ++i) {
                RoomMember member;
                member.peer = string(reader);
                const auto ready = reader.readU8();
                const auto seats = reader.readU8();
                if (ready > 1 || seats == 0 || seats > 4) {
                    disconnect(id);
                    return;
                }
                member.ready = ready != 0;
                const auto values = reader.readBytes(seats);
                member.seats.assign(values.begin(), values.end());
                room.members.push_back(std::move(member));
            }
            if (!reader.atEnd() || (!m_peer.empty() && peer != m_peer)) {
                disconnect(id);
                return;
            }
            m_peer = peer;
            link.peer = room.host;
            std::erase_if(m_cards, [&](const auto& row) {
                return std::ranges::find(room.members, row.first, &RoomMember::peer) ==
                       room.members.end();
            });
            m_room = std::move(room); // OnlineSession validates identities, seats and revision.
            m_dirty = true;
        } else if (kind == Kind::Card && !m_host && id == m_hostLink) {
            const auto peer = string(reader);
            const auto where = card(reader);
            if (!reader.atEnd()) {
                disconnect(id);
                return;
            }
            if (!where.empty()) {
                m_cards[peer] = where;
            }
        } else if (kind == Kind::Hello && !m_host && link.direct && link.peer.empty()) {
            // The guest that dialled names itself; it must be a member we are not linked to.
            const auto peer = string(reader);
            const bool member =
                std::ranges::find(m_room.members, peer, &RoomMember::peer) != m_room.members.end();
            const bool linked = std::ranges::any_of(
                m_links, [&](const auto& row) { return row.second.peer == peer; });
            if (!reader.atEnd() || !member || linked || peer == m_peer || peer == m_room.host) {
                disconnect(id);
                return;
            }
            link.peer = peer;
            link.hello = true;
            announce(id);
        } else if (kind == Kind::Ready && m_host && !link.peer.empty()) {
            const auto revision = number(reader);
            const auto value = reader.readU8();
            if (!reader.atEnd() || value > 1) {
                disconnect(id);
                return;
            }
            if (revision == m_room.revision && !m_room.started) {
                const auto member = std::ranges::find(m_room.members, link.peer, &RoomMember::peer);
                member->ready = value != 0;
                changed(false);
            }
        } else if (kind == Kind::Reject && !m_host && m_peer.empty()) {
            const auto reason = reader.readU8();
            if (!reader.atEnd() || reason < static_cast<u8>(Failure::Version) ||
                reason > static_cast<u8>(Failure::Started)) {
                fail(Failure::Protocol);
            } else {
                fail(static_cast<Failure>(reason));
            }
        } else {
            disconnect(id);
        }
    } catch (const FormatError&) {
        disconnect(id);
    }
}
RoomService::Update HostedRoom::poll() {
    if (!m_closed && m_failure == Failure::None) {
        for (const auto& event : m_wire.poll()) {
            if (event.type == PacketTransport::EventType::Connected) {
                if (const auto dialled = m_links.find(event.connection); dialled != m_links.end()) {
                    // Our dial to another guest came up: say who we are.
                    auto& link = dialled->second;
                    if (!link.direct || link.hello) {
                        continue;
                    }
                    auto bytes = packet(Kind::Hello);
                    string(bytes, m_peer);
                    queue(event.connection, std::move(bytes));
                    link.hello = true;
                    announce(event.connection);
                    continue;
                }
                if (m_links.size() >= 4) {
                    m_wire.close(event.connection);
                    continue;
                }
                auto& link = m_links[event.connection];
                if (!m_host && m_hostLink == 0) {
                    m_hostLink = event.connection;
                    auto bytes = packet(Kind::Join);
                    bytes.push_back(m_players);
                    string(bytes, m_build);
                    string(bytes, m_content);
                    card(bytes, m_wire.address());
                    queue(event.connection, std::move(bytes));
                } else if (!m_host) {
                    link.direct = true; // another guest dialled us; its hello names it
                }
            } else if (event.type == PacketTransport::EventType::Disconnected) {
                if (!m_host && event.connection == m_hostLink) {
                    fail(Failure::Connection);
                }
                disconnect(event.connection);
            } else if (m_links.contains(event.connection)) {
                message(event.connection, event.bytes);
            }
        }
        std::vector<Connection> closed;
        for (auto& [id, link] : m_links) {
            if ((link.peer.empty() || link.rejected || (link.direct && !link.hello)) &&
                std::chrono::steady_clock::now() - link.arrived > std::chrono::seconds(5)) {
                closed.push_back(id);
                continue;
            }
            while (!link.pending.empty()) {
                const auto sent = m_wire.send(id, link.pending.front(), Delivery::Reliable);
                if (sent == SendResult::Congested) {
                    break;
                }
                if (sent != SendResult::Sent) {
                    closed.push_back(id);
                    break;
                }
                link.pending.pop_front();
            }
        }
        for (const auto id : closed) {
            disconnect(id);
        }
    }
    Update update;
    update.closed = m_closed;
    if (m_failure != Failure::None) {
        switch (m_failure) {
        case Failure::Connection: update.error = "Connection to the room was lost"; break;
        case Failure::Version: update.error = "Game versions do not match"; break;
        case Failure::Assets: update.error = "Game assets do not match"; break;
        case Failure::Full: update.error = "Room has no free player slots"; break;
        case Failure::Started: update.error = "Game has already started"; break;
        case Failure::Protocol: update.error = "Room protocol does not match"; break;
        case Failure::None: break;
        }
    } else if (std::exchange(m_dirty, false)) {
        update.room = m_room;
        update.peer = m_peer;
    }
    return update;
}
bool HostedRoom::ready(u64 revision, bool value) {
    if (m_closed || m_failure != Failure::None || revision != m_room.revision || m_room.started) {
        return false;
    }
    if (m_host) {
        m_room.members.front().ready = value;
        changed(false);
        return true;
    }
    if (m_hostLink == 0 || !m_links.contains(m_hostLink)) {
        return false;
    }
    auto bytes = packet(Kind::Ready);
    number(bytes, revision);
    bytes.push_back(value ? 1 : 0);
    return queue(m_hostLink, std::move(bytes));
}
bool HostedRoom::start(u64 revision) {
    if (!m_host || m_closed || m_failure != Failure::None || revision != m_room.revision ||
        m_room.started || !std::ranges::all_of(m_room.members, &RoomMember::ready)) {
        return false;
    }
    m_room.started = true;
    changed(false);
    return true;
}
bool HostedRoom::settings(const RoomSettings& value) {
    if (!m_host || m_closed || m_failure != Failure::None || m_room.started || !value.valid()) {
        return false;
    }
    usize players = 0;
    for (const auto& member : m_room.members) {
        players += member.seats.size();
    }
    if (players > value.maxPlayers) {
        return false;
    }
    if (m_room.settings != value) {
        m_room.settings = value;
        changed(true);
    }
    return true;
}
void HostedRoom::leave() {
    if (!std::exchange(m_closed, true)) {
        for (const auto& [id, link] : m_links) {
            m_wire.close(id);
        }
        m_links.clear();
    }
}
bool HostedRoom::Transport::configurePeer(std::string identity, bool host) {
    return identity == m_room.m_peer && host == m_room.m_host;
}
bool HostedRoom::Transport::authorizePeers(std::span<const std::string> peers) {
    m_allowed = {peers.begin(), peers.end()};
    // A guest's host link is announced by connectPeer, so the session expects it.
    std::vector<Connection> ids;
    for (const auto& [id, link] : m_room.m_links) {
        if (m_room.m_host || link.direct) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        m_room.announce(id);
    }
    return true;
}
std::optional<PacketTransport::Connection>
HostedRoom::Transport::connectPeer(const std::string& peer) {
    if (m_room.m_host || !m_allowed.contains(peer)) {
        return std::nullopt;
    }
    if (peer == m_room.m_room.host) {
        for (auto& [id, link] : m_room.m_links) {
            if (link.peer == peer && !link.announced) {
                link.announced = true;
                m_room.m_events.push_back({EventType::Connected, id, {}, {}});
                return id;
            }
        }
        return std::nullopt;
    }
    // Another guest: dial its card. The hello goes out once the wire connects.
    if (std::ranges::any_of(m_room.m_links,
                            [&](const auto& row) { return row.second.peer == peer; })) {
        return std::nullopt;
    }
    const auto where = m_room.m_cards.find(peer);
    if (where == m_room.m_cards.end() || where->second.empty()) {
        return std::nullopt;
    }
    const auto id = m_room.m_wire.dial(where->second);
    if (!id || *id == 0 || m_room.m_links.contains(*id)) {
        return std::nullopt;
    }
    auto& link = m_room.m_links[*id];
    link.peer = peer;
    link.direct = true;
    return id;
}
std::optional<std::string> HostedRoom::Transport::peer(Connection connection) const {
    const auto found = m_room.m_links.find(connection);
    return found == m_room.m_links.end() ? std::nullopt : std::optional{found->second.peer};
}
PacketTransport::SendResult
HostedRoom::Transport::send(Connection connection, std::span<const u8> bytes, Delivery delivery) {
    const auto found = m_room.m_links.find(connection);
    if (found == m_room.m_links.end() || !found->second.announced ||
        !m_allowed.contains(found->second.peer)) {
        return SendResult::Disconnected;
    }
    if (bytes.empty() || bytes.size() > kMaxPacketBytes) {
        return SendResult::Invalid;
    }
    if (!found->second.pending.empty()) {
        return SendResult::Congested;
    }
    std::vector<u8> payload{static_cast<u8>(Kind::Game)};
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    return m_room.m_wire.send(connection, payload, delivery);
}
std::vector<PacketTransport::Event> HostedRoom::Transport::poll() {
    return std::exchange(m_room.m_events, {});
}
std::optional<PacketTransport::Statistics>
HostedRoom::Transport::statistics(Connection connection) const {
    return m_room.m_wire.statistics(connection);
}
void HostedRoom::Transport::close(Connection connection) {
    m_room.disconnect(connection);
}
} // namespace gdl::game
