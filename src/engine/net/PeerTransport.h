#pragma once

#include "engine/net/PacketTransport.h"

namespace gdl {
/** Room-authenticated gameplay links. Connection establishment belongs to the
 * provider adapter; no vendor types or relay credentials escape here. */
class PeerTransport : public PacketTransport {
public:
    virtual bool configurePeer(std::string identity, bool host) = 0;
    virtual bool authorizePeers(std::span<const std::string> peers) = 0;
    virtual std::optional<Connection> connectPeer(const std::string& peer) = 0;
    virtual std::optional<std::string> peer(Connection connection) const = 0;
};
} // namespace gdl
