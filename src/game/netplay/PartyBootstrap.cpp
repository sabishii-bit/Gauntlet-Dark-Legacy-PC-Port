#include "game/netplay/PartyBootstrap.h"

#include <algorithm>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDPS");
constexpr u8 kVersion = 1;
constexpr usize kHeader = 8;
enum class Kind : u8 { Profile, Seal, Acknowledge };
std::vector<u8> packet(Kind kind, u8 seat = 0, std::span<const u8> profile = {}) {
    std::vector<u8> bytes;
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(kMagic >> shift));
    }
    bytes.insert(bytes.end(), {kVersion, static_cast<u8>(kind), seat, 0});
    bytes.insert(bytes.end(), profile.begin(), profile.end());
    return bytes;
}
} // namespace

bool PartyBootstrap::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}
bool PartyBootstrap::validPacket(std::span<const u8> bytes) {
    if (!recognizes(bytes) || bytes.size() < kHeader || bytes[4] != kVersion || bytes[7] != 0) {
        return false;
    }
    if (bytes[5] == static_cast<u8>(Kind::Profile)) {
        return bytes[6] < InputCommand::kSeats &&
               CharacterProfilePacket::decode(bytes.subspan(kHeader)).has_value();
    }
    return bytes[5] <= static_cast<u8>(Kind::Acknowledge) && bytes[6] == 0 &&
           bytes.size() == kHeader;
}

bool PartyBootstrap::open(u8 local, const MatchOwners& owners, std::span<const MatchLink> links,
                          std::span<const CharacterProfile> selections) {
    if (opened() || local == 0 || local > 4 ||
        std::ranges::any_of(owners, [](u8 owner) { return owner > 4; }) ||
        std::ranges::find(owners, 1) == owners.end() ||
        std::ranges::count(owners, local) != static_cast<s64>(selections.size()) ||
        selections.empty()) {
        return false;
    }
    PartyBootstrap pending;
    pending.m_local = local;
    pending.m_owners = owners;
    for (const auto& link : links) {
        if (link.peer == 0 || link.peer > 4 || link.peer == local || link.connection == 0 ||
            pending.m_links[link.peer] != 0 ||
            std::ranges::find(pending.m_links, link.connection) != pending.m_links.end() ||
            std::ranges::find(owners, link.peer) == owners.end() ||
            (local != 1 && link.peer != 1)) {
            return false;
        }
        pending.m_links[link.peer] = link.connection;
    }
    for (const u8 owner : owners) {
        if (owner != 0 && owner != local && (local == 1 || owner == 1) &&
            pending.m_links[owner] == 0) {
            return false;
        }
    }
    usize next = 0;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] != local) {
            continue;
        }
        const auto encoded = CharacterProfilePacket::encode(selections[next]);
        if (!encoded) {
            return false;
        }
        pending.m_party[seat] = selections[next++];
        pending.m_encoded[seat] = *encoded;
        if (local != 1) {
            pending.m_outgoing.push_back(
                {pending.m_links[1], packet(Kind::Profile, static_cast<u8>(seat), *encoded)});
        }
    }
    pending.seal();
    *this = std::move(pending);
    return true;
}
bool PartyBootstrap::allProfiles() const {
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if ((m_owners[seat] != 0) != m_party[seat].has_value()) {
            return false;
        }
    }
    return true;
}
void PartyBootstrap::seal() {
    if (m_local != 1 || m_sealed || !allProfiles()) {
        return;
    }
    m_sealed = true;
    m_complete = true; // A host with only local seats needs no remote acknowledgement.
    for (usize peer = 2; peer < m_links.size(); ++peer) {
        if (m_links[peer] == 0) {
            continue;
        }
        m_complete = false;
        for (usize seat = 0; seat < m_party.size(); ++seat) {
            if (m_party[seat]) {
                m_outgoing.push_back(
                    {m_links[peer], packet(Kind::Profile, static_cast<u8>(seat), m_encoded[seat])});
            }
        }
        m_outgoing.push_back({m_links[peer], packet(Kind::Seal)});
    }
}
bool PartyBootstrap::receive(PacketTransport::Connection connection, std::span<const u8> bytes) {
    if (!opened() || connection == 0 || !validPacket(bytes)) {
        return false;
    }
    const auto peer = static_cast<u8>(std::ranges::find(m_links, connection) - m_links.begin());
    if (peer == m_links.size()) {
        return false;
    }
    const auto kind = static_cast<Kind>(bytes[5]);
    if (kind == Kind::Profile) {
        const u8 seat = bytes[6];
        if (m_owners[seat] == 0 || (m_local == 1 && m_owners[seat] != peer)) {
            return false;
        }
        const auto payload = bytes.subspan(kHeader);
        if (m_party[seat]) {
            // Replays are harmless; replacing a local selection or a frozen peer is not.
            return std::ranges::equal(m_encoded[seat], payload);
        }
        m_party[seat] = CharacterProfilePacket::decode(payload);
        m_encoded[seat].assign(payload.begin(), payload.end());
        seal();
        return true;
    }
    if (kind == Kind::Seal) {
        if (m_local == 1 || peer != 1 || m_sealed || !allProfiles()) {
            return false;
        }
        m_sealed = true;
        m_complete = true;
        m_outgoing.push_back({m_links[1], packet(Kind::Acknowledge)});
        return true;
    }
    if (m_local != 1 || !m_sealed || peer == 1) {
        return false;
    }
    m_acknowledged[peer] = true;
    m_complete = true;
    for (usize i = 2; i < m_links.size(); ++i) {
        m_complete = m_complete && (m_links[i] == 0 || m_acknowledged[i]);
    }
    return true;
}
bool PartyBootstrap::flush(PacketTransport& transport) {
    while (!m_outgoing.empty()) {
        const auto& delivery = m_outgoing.front();
        const auto result = transport.send(delivery.connection, delivery.bytes,
                                           PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return true;
        }
        if (result != PacketTransport::SendResult::Sent) {
            return false;
        }
        m_outgoing.pop_front();
    }
    return true;
}
} // namespace gdl::game
