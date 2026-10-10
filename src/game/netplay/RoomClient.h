#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "game/netplay/RoomService.h"

namespace gdl::game {

class RoomServiceError : public std::runtime_error {
public:
    explicit RoomServiceError(s32 status);
    s32 status() const { return m_status; }

private:
    s32 m_status;
};

/** Blocking coordinator client, owned by a networking worker (or headless test),
 * NEVER the game/update/render thread. No gameplay packets or saves travel here.
 * HTTPS verifies certificates; plain HTTP is restricted to numeric IPv4 loopback.
 * Failures throw std::runtime_error; callers must surface them and end/retry setup.
 * No automatic create/join retry: a lost response could have admitted a member.
 * Such reservations expire server-side instead of accidentally duplicating seats. */
class RoomClient {
    struct Impl;

public:
    // Bump admission compatibility whenever a gameplay wire contract changes.
    // Version 4 requires authoritative per-seat checkpoints before stage travel.
    static constexpr u32 kProtocol = 7;
    explicit RoomClient(const std::string& endpoint);
    ~RoomClient();
    RoomClient(const RoomClient&) = delete;
    RoomClient& operator=(const RoomClient&) = delete;
    RoomClient(RoomClient&&) = delete;
    RoomClient& operator=(RoomClient&&) = delete;

    RoomSnapshot enter(const std::string& code, u8 localPlayers, const std::string& build,
                       const std::string& contentHash);
    RoomSnapshot poll();
    RoomSnapshot ready(u64 revision, bool ready);
    RoomSnapshot start(u64 revision);
    void leave();
    void sendSignal(const PeerTransport::Signal& signal);
    const std::string& peer() const;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace gdl::game
