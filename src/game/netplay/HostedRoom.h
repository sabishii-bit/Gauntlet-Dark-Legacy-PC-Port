#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <set>

#include "engine/net/PeerTransport.h"

#include "game/netplay/RoomService.h"

namespace gdl::game {
/** Local-host room authority over an already authenticated packet adapter.
 * Implements the same admission/roster boundaries as a managed room service.
 * No HTTP server, public directory, gameplay rules or vendor API lives here. */
class HostedRoom final : public RoomService {
public:
    enum class Failure : u8 { None, Connection, Version, Assets, Full, Started, Protocol };
    HostedRoom(PacketTransport& wire, bool host, u8 players, std::string build,
               std::string content);
    ~HostedRoom() override;
    HostedRoom(const HostedRoom&) = delete;
    HostedRoom& operator=(const HostedRoom&) = delete;
    HostedRoom(HostedRoom&&) = delete;
    HostedRoom& operator=(HostedRoom&&) = delete;
    PeerTransport& transport() { return m_transport; }
    Failure failure() const { return m_failure; }
    Update poll() override;
    bool ready(u64 revision, bool value = true) override;
    bool start(u64 revision) override;
    bool settings(const RoomSettings& value);
    void leave() override;

private:
    using Connection = PacketTransport::Connection;
    using Event = PacketTransport::Event;
    using SendResult = PacketTransport::SendResult;
    using Delivery = PacketTransport::Delivery;
    class Transport final : public PeerTransport {
    public:
        explicit Transport(HostedRoom& room) : m_room(room) {}
        bool configurePeer(std::string identity, bool host) override;
        bool authorizePeers(std::span<const std::string> peers) override;
        std::optional<Connection> connectPeer(const std::string& peer) override;
        std::optional<std::string> peer(Connection connection) const override;
        SendResult send(Connection connection, std::span<const u8> bytes,
                        Delivery delivery) override;
        std::vector<Event> poll() override;
        std::optional<Statistics> statistics(Connection connection) const override;
        void close(Connection connection) override;

    private:
        HostedRoom& m_room;
        std::set<std::string> m_allowed;
    };
    struct Link {
        std::string peer;
        std::deque<std::vector<u8>> pending;
        bool announced = false;
        bool rejected = false;
        std::chrono::steady_clock::time_point arrived = std::chrono::steady_clock::now();
    };
    void message(Connection id, std::span<const u8> bytes);
    void disconnect(Connection id);
    void changed(bool membership);
    bool queue(Connection id, std::vector<u8> bytes);
    void fail(Failure failure);
    PacketTransport& m_wire;
    bool m_host;
    u8 m_players;
    std::string m_build;
    std::string m_content;
    std::string m_peer;
    RoomSnapshot m_room;
    std::map<Connection, Link> m_links;
    std::vector<Event> m_events;
    bool m_dirty = false;
    bool m_closed = false;
    Failure m_failure = Failure::None;
    Transport m_transport;
};
} // namespace gdl::game
