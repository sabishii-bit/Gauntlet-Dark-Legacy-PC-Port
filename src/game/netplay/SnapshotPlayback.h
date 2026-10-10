#pragma once

#include <deque>

#include "engine/net/PacketTransport.h"

#include "game/netplay/MotionSnapshot.h"

namespace gdl::game {

/** Client-only presentation history, bounded and independent of rendering and
 * gameplay callbacks. Never extrapolates lost state or runs client-side physics.
 * The caller supplies its delayed host-tick clock; this is not clock sync or
 * local-player prediction. Discrete presence/identity and cuts switch at their
 * snapshot tick, not when the packet happens to arrive. */
class SnapshotPlayback {
public:
    enum class Admission : u8 { Accepted, WrongHost, WrongEpoch, Invalid, Stale };
    static constexpr usize kHistory = 32;

    /** Bind from an authenticated start/travel control message, never from an
     * unreliable snapshot. Same-host epochs must increase. To start a new room,
     * clear first; connection IDs are local transport lifetimes, not wire IDs. */
    bool begin(PacketTransport::Connection host, u64 epoch);
    void clear();
    Admission receive(PacketTransport::Connection sender, std::span<const u8> bytes);
    /** Sample absolute host tick + fraction in [0,1). Outside retained history,
     * hold the nearest complete state. Metadata is that of the lower snapshot;
     * the interpolated result is for display, not transmission or simulation. */
    std::optional<MotionSnapshot> sample(u64 tick, f32 fraction = 0) const;
    const MotionSnapshot* latest() const;
    usize size() const { return m_history.size(); }

private:
    PacketTransport::Connection m_host = 0;
    u64 m_epoch = 0;
    std::deque<MotionSnapshot> m_history;
};

} // namespace gdl::game
