#pragma once

#include <algorithm>
#include <array>

#include "engine/core/Types.h"
#include "engine/net/PacketTransport.h"

#include "fixtures/RecordingPacketTransport.h"

namespace gdl::test {
/** Peer 1 hosts; a guest reaches it on `host`, and it reaches guest p on p + guestOffset. */
struct StarLinks {
    PacketTransport::Connection host = 99;
    s64 guestOffset = 100;

    constexpr PacketTransport::Connection guest(u8 peer) const {
        return static_cast<PacketTransport::Connection>(static_cast<s64>(peer) + guestOffset);
    }
    /** The peer a packet `source` sent on `connection` arrives at. */
    constexpr u8 receiver(u8 source, PacketTransport::Connection connection) const {
        return source == 1 ? static_cast<u8>(static_cast<s64>(connection) - guestOffset) : u8{1};
    }
    /** The connection the receiver sees `source`'s packet on. */
    constexpr PacketTransport::Connection sender(u8 source) const {
        return source == 1 ? host : guest(source);
    }
};

/** Up to four peers in a star, each with its own wire. Slot 0 is unused, so a peer's number
 * is its slot. */
template <class Wire = RecordingPacketTransport> struct PacketStar {
    static constexpr u8 kPeers = 4;
    static constexpr usize kSlots = kPeers + 1;
    static constexpr usize kSeats = 4;
    enum class Order : u8 { AsSent, NewestFirst };
    using Packet = typename Wire::Packet; ///< what a wire keeps of each send

    std::array<u8, kSeats> owners; ///< the peer playing each seat, zero for none
    StarLinks star;
    std::array<Wire, kSlots> wires;

    explicit PacketStar(std::array<u8, kSeats> roster, StarLinks links = {})
        : owners(roster), star(links) {}

    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
    /** Takes what `source` sent and calls receive(source, target, connection, packet) for each
     * packet in `order`, `connection` being the one the target sees it on. */
    template <class Receive>
    void deliver(u8 source, const Receive& receive, Order order = Order::AsSent) {
        auto packets = wires[source].take();
        if (order == Order::NewestFirst) {
            std::ranges::reverse(packets);
        }
        for (const auto& packet : packets) {
            receive(source, star.receiver(source, packet.connection), star.sender(source), packet);
        }
    }
    /** `passes` rounds over the present peers 1..4: flush(peer, wire), then deliver. */
    template <class Flush, class Receive>
    void pump(u32 passes, const Flush& flush, const Receive& receive) {
        for (u32 pass = 0; pass < passes; ++pass) {
            for (u8 peer = 1; peer <= kPeers; ++peer) {
                if (present(peer)) {
                    flush(peer, wires[peer]);
                    deliver(peer, receive);
                }
            }
        }
    }
};
} // namespace gdl::test
