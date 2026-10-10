#pragma once

#include <array>
#include <map>
#include <optional>

#include "game/netplay/InputCommand.h"

namespace gdl::game {

/** Host-side admission and playout, independent of sockets and local input devices.
 * Only the host assigns seats and advances ticks. Call from the simulation thread;
 * transport callbacks must enqueue messages instead of mutating this concurrently.
 * This is authoritative input delivery, NOT a deterministic lockstep simulation. */
class InputTimeline {
public:
    using Peer = u64;
    using Frame = std::array<InputCommand, InputCommand::kSeats>;
    enum class Admission : u8 {
        Accepted,
        Invalid,
        WrongEpoch,
        WrongOwner,
        WrongGrant,
        TooLate,
        TooEarly,
        Duplicate,
    };
    // Bounds in simulation ticks. At the current 60 Hz update rate these allow
    // two seconds of queued inputs and 100 ms of held-input concealment.
    static constexpr u64 kMaxAhead = 120;
    static constexpr u64 kHoldTicks = 6;

    /** Zero denotes no owner. Reassigning, even to the same peer, revokes old packets. */
    bool assign(usize seat, Peer peer);
    void disconnect(Peer peer);
    Peer owner(usize seat) const;
    u32 grant(usize seat) const;
    u64 epoch() const { return m_epoch; }
    u64 tick() const { return m_tick; }
    /** Reject all old-context inputs, keeping the roster. Never reuse an epoch. */
    void beginEpoch();
    Admission submit(Peer sender, const InputCommand& command);
    /** Consume one tick, once. Missing inputs repeat only held state briefly;
     * one-shot actions are never inferred or repeated. Late edges are discarded. */
    Frame advance();

private:
    struct Seat {
        Peer owner = 0;
        u32 grant = 0;
        std::map<u64, InputCommand> pending;
        std::optional<InputCommand> previous;
    };
    std::array<Seat, InputCommand::kSeats> m_seats;
    u64 m_epoch = 1;
    u64 m_tick = 0;
};

} // namespace gdl::game
