#pragma once

#include "engine/net/PacketTransport.h"

namespace gdl {
/** Room-authenticated peer transport. Signaling is carried by the room service,
 * independently of the low-latency gameplay channel. No vendor types escape here. */
class PeerTransport : public PacketTransport {
public:
    struct Signal {
        std::string peer;
        std::vector<u8> bytes;
    };
    static constexpr usize kMaxSignalBytes = 8192;

    virtual bool configurePeer(std::string identity, bool host) = 0;
    virtual bool authorizePeers(std::span<const std::string> peers) = 0;
    virtual std::optional<Connection> connectPeer(const std::string& peer) = 0;
    virtual std::optional<std::string> peer(Connection connection) const = 0;
    virtual std::vector<Signal> takeSignals() = 0;
    virtual bool receiveSignal(const Signal& signal) = 0;
};
} // namespace gdl
