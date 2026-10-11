#pragma once

#include <memory>

#include "engine/net/PacketTransport.h"

namespace gdl {
/** Provider-neutral connection lifecycle. Invitations are opaque, private and
 * scoped to one host session. No provider types leak into gameplay or rooms.
 * Its wire accepts kMaxPacketBytes + 1 bytes, including the room envelope; the
 * admitted PeerTransport exposed to gameplay retains the 1200-byte limit. */
class SessionTransport : public PacketTransport {
public:
    enum class Phase : u8 { Opening, Ready, Failed };
    struct Options {
        bool localOnly = false; ///< Offline tests; no relay or discovery access.
        std::string invitation; ///< Empty hosts; nonempty joins that host.
    };
    virtual Phase phase() const = 0;
    virtual std::string invitation() const = 0;
    virtual std::string error() const = 0;
};
/** Composition point for the selected provider. Another service or self-hosted
 * adapter can replace this factory without changing the room or gameplay. */
std::unique_ptr<SessionTransport> openSessionTransport(const SessionTransport::Options& options,
                                                       std::string& error);
} // namespace gdl
