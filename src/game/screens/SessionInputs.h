#pragma once

#include <array>

#include "game/netplay/InputTimeline.h"
#include "game/screens/PlayInput.h"

namespace gdl::game {

/** The listen host's input boundary. Offline play owns all four seats locally.
 * A remote peer can own multiple seats, without knowing that machine's device IDs.
 * This does not run on a replica client: snapshots, not this class, drive replicas.
 * Character selection/shop transactions and room discovery are separate protocols. */
class SessionInputs {
public:
    static constexpr InputTimeline::Peer kLocalPeer = 1;
    using Frame = std::array<PlayInput, InputCommand::kSeats>;

    SessionInputs();
    InputTimeline& timeline() { return m_timeline; }
    const InputTimeline& timeline() const { return m_timeline; }
    void beginEpoch() { m_timeline.beginEpoch(); }
    /** Read local controls and consume remote commands at the same tick. Local
     * pointer/text input remains local, lossless, and valid only during this call. */
    Frame advance(const Frame& local);

    static InputCommand command(const PlayInput& input, u64 epoch, u64 tick, u32 grant, u8 seat);
    static PlayInput playInput(const InputCommand& command);

private:
    InputTimeline m_timeline;
};

} // namespace gdl::game
