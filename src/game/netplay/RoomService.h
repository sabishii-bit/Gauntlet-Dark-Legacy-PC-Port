#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {
struct RoomSettings {
    u8 maxPlayers = 4;
    u8 difficulty = 1;   ///< easy, normal, hard
    u8 friendlyFire = 0; ///< normal, stun, hurt
    u8 camera = 0;       ///< shared (the classic one camera for the party), solo (one each)
    u8 mechanics = 0;    ///< the mechanics the room switches on, a bit each in catalog order
    bool valid() const {
        return maxPlayers >= 1 && maxPlayers <= 4 && difficulty <= 2 && friendlyFire <= 2 &&
               camera <= 1;
    }
    bool operator==(const RoomSettings&) const = default;
};
struct RoomMember {
    std::string endpointId;
    std::vector<u8> seats;
    bool ready = false;
    bool operator==(const RoomMember&) const = default;
};
struct RoomSnapshot {
    std::string code;
    std::string hostEndpointId;
    u64 revision = 0;
    bool started = false;
    std::vector<RoomMember> members;
    RoomSettings settings{}; // default rules for existing room-service adapters
};

/** Nonblocking room mailbox. Implementations own credentials/network workers;
 * the gameplay thread only consumes admission updates and submits bounded work. */
class RoomService {
public:
    struct Update {
        std::optional<RoomSnapshot> room;
        std::string endpointId;
        std::string error;
        bool closed = false;
    };
    virtual ~RoomService() = default;
    virtual Update poll() = 0;
    virtual bool ready(u64 revision, bool value = true) = 0;
    virtual bool start(u64 revision) = 0;
    virtual void leave() = 0;
};
} // namespace gdl::game
