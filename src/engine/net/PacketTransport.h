#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Message transport only. A connection ID identifies a local connection lifetime,
 * NOT an authenticated player. Room admission must establish that separately.
 * Poll and send on the owning thread; no game callbacks run on library threads. */
class PacketTransport {
public:
    using Connection = u64;
    static constexpr usize kMaxPacketBytes = 1200;
    static constexpr usize kMaxConnections = 4;
    static constexpr usize kReceiveBudget = 128;
    static constexpr s32 kSendBufferBytes = 64 * 1024;

    enum class Delivery : u8 { Unreliable, Reliable };
    enum class SendResult : u8 { Sent, Congested, Disconnected, Invalid };
    enum class EventType : u8 { Connected, Disconnected, Message };
    struct Event {
        EventType type;
        Connection connection;
        std::vector<u8> bytes;
        std::string reason;
    };
    struct Statistics {
        s32 pingMs = -1;
        s32 pendingBytes = 0;
        s64 queueMicroseconds = 0;
    };

    virtual ~PacketTransport() = default;
    PacketTransport(const PacketTransport&) = delete;
    PacketTransport& operator=(const PacketTransport&) = delete;
    PacketTransport(PacketTransport&&) = delete;
    PacketTransport& operator=(PacketTransport&&) = delete;
    /** Reliable control is ordered. Unreliable state is never retransmitted;
     * callers bound/coalesce pending state while completing any partly sent
     * checkpoint, rather than growing a queue of obsolete updates. */
    virtual SendResult send(Connection connection, std::span<const u8> bytes,
                            Delivery delivery) = 0;
    virtual std::vector<Event> poll() = 0;
    virtual std::optional<Statistics> statistics(Connection connection) const = 0;
    virtual void close(Connection connection) = 0;

protected:
    PacketTransport() = default;
};

} // namespace gdl
