#pragma once

#include <memory>

#include "engine/net/PeerTransport.h"

namespace gdl {

/** Private GameNetworkingSockets adapter. No vendor headers escape this boundary.
 * One instance owns the library runtime per process; creating/destroying it starts/
 * stops its worker threads. Offline play never needs to create one.
 * Room membership authorizes P2P identities. Encrypted raw IP connections alone
 * are NOT authenticated internet netplay. */
class GnsTransport final : public PeerTransport {
    struct Impl;
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

public:
    struct Simulation {
        s32 lagMs = 0;
        f32 lossPercent = 0;
        bool trace = false; // development diagnostics; includes local/peer network addresses
    };
    static std::unique_ptr<GnsTransport> create(Simulation simulation, std::string& error);
    explicit GnsTransport(ConstructionKey /*unused*/, std::unique_ptr<Impl> impl);
    ~GnsTransport() override;
    GnsTransport(const GnsTransport&) = delete;
    GnsTransport& operator=(const GnsTransport&) = delete;
    GnsTransport(GnsTransport&&) = delete;
    GnsTransport& operator=(GnsTransport&&) = delete;

    /** Choose an available dynamic loopback port, with bounded bind retries. */
    std::optional<u16> listenLoopback();
    std::optional<Connection> connectLoopback(u16 port);
    /** Local ICE candidates only for now; never contacts public STUN/TURN servers.
     * Call before opening links. Identity is a room-issued 32-digit lowercase hex ID. */
    bool configurePeer(std::string identity, bool host) override;
    /** Replace the authenticated room roster. Removed peers are disconnected. */
    bool authorizePeers(std::span<const std::string> peers) override;
    std::optional<Connection> connectPeer(const std::string& peer) override;
    std::optional<std::string> peer(Connection connection) const override;
    /** Bounded mailbox: callbacks only enqueue, never perform HTTP or touch gameplay.
     * The caller must route messages through authenticated room signaling. */
    std::vector<Signal> takeSignals() override;
    bool receiveSignal(const Signal& signal) override;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override;
    std::vector<Event> poll() override;
    std::optional<Statistics> statistics(Connection connection) const override;
    void close(Connection connection) override;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace gdl
