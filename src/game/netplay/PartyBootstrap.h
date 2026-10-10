#pragma once

#include "game/netplay/CharacterProfile.h"
#include "game/netplay/MatchSession.h"

namespace gdl::game {
/** One immutable party exchange for a frozen, authenticated room roster. Guests
 * submit only their own seats; the host relays the complete party in seat order.
 * Reliable per-connection ordering makes the final acknowledgement certify all
 * preceding profiles. No scene can start until every guest acknowledges.
 * Reconnects require a new room/transport lifetime, not reused socket IDs. */
class PartyBootstrap {
public:
    using Party = MatchParty;
    bool open(u8 local, const MatchOwners& owners, std::span<const MatchLink> links,
              std::span<const CharacterProfile> selections);
    bool receive(PacketTransport::Connection connection, std::span<const u8> bytes);
    bool flush(PacketTransport& transport);
    bool opened() const { return m_local != 0; }
    bool complete() const { return m_complete; }
    const Party& party() const { return m_party; }
    static bool recognizes(std::span<const u8> bytes);
    static bool validPacket(std::span<const u8> bytes);

private:
    struct Delivery {
        PacketTransport::Connection connection;
        std::vector<u8> bytes;
    };
    bool allProfiles() const;
    void seal();
    u8 m_local = 0;
    MatchOwners m_owners{};
    std::array<PacketTransport::Connection, 5> m_links{};
    Party m_party;
    std::array<std::vector<u8>, InputCommand::kSeats> m_encoded;
    std::array<bool, 5> m_acknowledged{};
    bool m_sealed = false;
    bool m_complete = false;
    std::deque<Delivery> m_outgoing;
};
} // namespace gdl::game
