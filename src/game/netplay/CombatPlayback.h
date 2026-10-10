#pragma once

#include "game/netplay/CombatReplica.h"
#include "game/netplay/SnapshotPlayback.h"

namespace gdl::game {

/** Delayed client display, not a simulation. Motion, camera, health and actor
 * presence share one snapshot clock. Only complete host checkpoints enter the
 * history; missing updates hold the last state without inventing hits or deaths. */
class CombatPlayback {
public:
    bool begin(PacketTransport::Connection host, u64 epoch);
    void clear();
    CombatReplica::Admission receive(PacketTransport::Connection sender, std::span<const u8> bytes);
    std::optional<CombatSnapshot> sample(u64 tick, f32 fraction = 0) const;
    const CombatSnapshot* latest() const { return m_receiver.latest(); }
    usize size() const { return m_history.size(); }

private:
    CombatReplica m_receiver;
    SnapshotPlayback m_motion;
    std::deque<CombatSnapshot> m_history;
};
} // namespace gdl::game
