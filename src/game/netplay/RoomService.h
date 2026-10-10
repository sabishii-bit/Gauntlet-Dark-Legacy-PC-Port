#pragma once

#include "engine/net/PeerTransport.h"

namespace gdl::game {
struct RoomMember {
    std::string peer;
    std::vector<u8> seats;
    bool ready = false;
    bool operator==(const RoomMember&) const = default;
};
struct RoomSnapshot {
    std::string code;
    std::string host;
    u64 revision = 0;
    bool started = false;
    std::vector<RoomMember> members;
    std::vector<PeerTransport::Signal> signals;
};

/** Nonblocking room mailbox. Implementations own credentials/network workers;
 * the gameplay thread only consumes admission updates and submits bounded work. */
class RoomService {
public:
    struct Update {
        std::optional<RoomSnapshot> room;
        std::string peer;
        std::string error;
        bool closed = false;
    };
    virtual ~RoomService() = default;
    virtual Update poll() = 0;
    virtual bool send(PeerTransport::Signal signal) = 0;
    virtual bool ready(u64 revision, bool value = true) = 0;
    virtual bool start(u64 revision) = 0;
    virtual void leave() = 0;
};
} // namespace gdl::game
