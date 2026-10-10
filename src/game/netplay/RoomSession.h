#pragma once

#include <memory>

#include "game/netplay/RoomService.h"

namespace gdl::game {

/** Asynchronous room-service boundary. HTTP and credential ownership live on a
 * dedicated worker. poll/send/ready/start/leave only touch bounded mailboxes;
 * they never wait for network I/O. Destruction joins the worker (bounded HTTP
 * timeout), so destroy during session teardown, not a gameplay frame. */
class RoomSession final : public RoomService {
    struct Impl;

public:
    RoomSession(std::string endpoint, std::string code, u8 localPlayers, std::string build,
                std::string contentHash);
    ~RoomSession() override;
    RoomSession(const RoomSession&) = delete;
    RoomSession& operator=(const RoomSession&) = delete;
    RoomSession(RoomSession&&) = delete;
    RoomSession& operator=(RoomSession&&) = delete;

    Update poll() override;
    bool send(PeerTransport::Signal signal) override;
    bool ready(u64 revision, bool value = true) override;
    bool start(u64 revision) override;
    void leave() override;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace gdl::game
