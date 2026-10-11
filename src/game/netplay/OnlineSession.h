#pragma once

#include <map>

#include "engine/net/PeerTransport.h"

#include "game/netplay/MatchSession.h"
#include "game/netplay/PartyBootstrap.h"
#include "game/netplay/RoomService.h"

namespace gdl::game {
/** Application-side room/peer/match lifecycle. The same driver runs in network
 * tests and the game; it has no HTTP, vendor, renderer or save I/O dependencies.
 * Pump on the owning thread even while loading/paused. RoomService and
 * PeerTransport must outlive this object; their destruction belongs to teardown.
 * One instance represents one room lifetime, with a frozen roster after start. */
class OnlineSession {
public:
    enum class Phase : u8 { Joining, Lobby, Starting, Active, Leaving, Closed, Failed };
    enum class Failure : u8 {
        None,
        Admission,
        Service,
        Transport,
        Protocol,
        RosterChanged,
        Timeout,
        Match
    };
    static constexpr f64 kAdmissionTimeout = 10;
    static constexpr f64 kConnectionTimeout = 20;
    static constexpr f64 kStartTimeout = 30;
    static constexpr f64 kLeaveTimeout = 5;

    OnlineSession(RoomService& service, PeerTransport& transport, bool host, u8 localPlayers);
    ~OnlineSession();
    OnlineSession(const OnlineSession&) = delete;
    OnlineSession& operator=(const OnlineSession&) = delete;
    OnlineSession(OnlineSession&&) = delete;
    OnlineSession& operator=(OnlineSession&&) = delete;

    void update(f64 seconds);
    /** Local-device order, mapped to the room's assigned seats on start. Select
     * before ready; unready before changing an existing lobby selection. */
    bool select(std::span<const CharacterProfile> selections);
    bool ready(bool value = true);
    bool start();
    void leave();
    Phase phase() const { return m_phase; }
    Failure failure() const { return m_failure; }
    bool host() const { return m_host; }
    bool connected() const;
    const RoomSnapshot* room() const { return m_room ? &*m_room : nullptr; }
    std::span<const u8> seats() const { return m_seats; }
    const PartyBootstrap::Party* party() const {
        return m_party.complete() ? &m_party.party() : nullptr;
    }
    MatchSession& match() { return m_match; }
    const MatchSession& match() const { return m_match; }

private:
    using Connection = PacketTransport::Connection;
    struct ReadyRequest {
        u64 revision;
        bool value;
    };
    bool admit(RoomSnapshot room, const std::string& peer);
    void event(const PacketTransport::Event& event);
    void openMatch();
    void fail(Failure reason);
    void closeLinks();
    std::vector<std::string> allowed() const;

    RoomService& m_service;
    PeerTransport& m_transport;
    bool m_host;
    u8 m_localPlayers;
    Phase m_phase = Phase::Joining;
    Failure m_failure = Failure::None;
    std::optional<RoomSnapshot> m_room;
    std::string m_peer;
    std::vector<u8> m_seats;
    std::map<std::string, Connection> m_links;
    std::optional<Connection> m_connecting;
    std::optional<ReadyRequest> m_ready;
    std::optional<PacketTransport::Event> m_prepare;
    std::vector<PacketTransport::Event> m_earlyParty;
    std::vector<CharacterProfile> m_selections;
    PartyBootstrap m_party;
    f64 m_wait = 0;
    MatchSession m_match;
};
} // namespace gdl::game
