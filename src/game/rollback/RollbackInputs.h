#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "game/rollback/InputCommand.h"

namespace gdl::game {

/** Input history for one independently simulated peer. Actual commands are
 * immutable evidence; used predictions are disposable. Gameplay snapshots,
 * transport, device polling, output confirmation and scene loading are callers'
 * responsibilities. All methods run on the simulation owner thread.
 *
 * tick() names the next pre-update state. On a mismatch, restore that state's
 * native snapshot before rewind(); replay never samples local devices again.
 * Only confirmedTick() can authorize irreversible effects or scene changes. */
class RollbackInputs {
public:
    using Owners = std::array<u8, InputCommand::kSeats>;
    using Grants = std::array<u32, InputCommand::kSeats>;
    using Commands = std::array<InputCommand, InputCommand::kSeats>;
    struct Frame {
        Commands commands{};
        u8 predictedSeats = 0;
    };
    enum class Admission : u8 {
        Accepted,
        Duplicate,
        Stale,
        Invalid,
        WrongPeer,
        WrongEpoch,
        WrongGrant,
        TooEarly,
        Conflict
    };
    static constexpr u64 kPredictionTicks = 8;
    static constexpr u64 kRedundantTicks = 8;
    static constexpr u64 kHistoryTicks = 128;
    static constexpr u64 kMaxAhead = 120;
    static constexpr u64 kPeerSkew = 2;
    static constexpr u8 kMaxDelay = 4;
    /** The round trips under which a startup delay of nought, one, two and three ticks
     * still has a peer's input in hand when its tick comes: a tick a way for half the trip
     * and a little over for the sender's pacing. Prediction covers only the jitter past
     * that, so a correction, which replays every tick back to the mispredicted one, is
     * rare; left to prediction (a two-tick cap over an eighty-millisecond trip), a party
     * on the move corrected most ticks and the replays cost more than the ticks. */
    static constexpr s32 kNoDelayPingMs = 20;
    static constexpr s32 kOneTickPingMs = 40;
    static constexpr s32 kTwoTickPingMs = 70;
    static constexpr s32 kThreeTickPingMs = 105;
    /** The local input delay a measured round trip calls for; an unknown ping keeps the
     * one-tick default. */
    static u8 leadForPing(s32 pingMs);

    /** Remote progress initially has unknown transit age. Use the conservative
     * eight-tick pacing allowance until its first report, without extending the
     * separate eight-unconfirmed-tick prediction window. */
    bool open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants, u8 delay = 1);
    bool sampleReady() const;
    /** Exactly one complete local frame at sampleTick(). Edges may be accumulated
     * by the input adapter while this is backpressured, never merged after admission. */
    bool sample(std::span<const InputCommand> commands);
    /** Authenticate peer outside this class. Each represented tick must include
     * all that peer's occupied seats; up to eight ticks may arrive out of order.
     * Invalid/conflicting batches change no history. An actual/predicted mismatch
     * is Accepted and schedules correction, not a protocol conflict. */
    Admission submit(u8 authenticatedPeer, std::span<const InputCommand> commands);
    bool ready() const;
    std::optional<Frame> advance();
    /** Accept only the earliest pending correction after the caller restores its
     * native pre-tick snapshot. Actual inputs and unsent device edges are untouched. */
    bool rewind(u64 tick);
    /** Finish an active replay at the current next tick after executing at least
     * one corrected frame, when the native scene reached an earlier terminal.
     * No future simulation is fabricated. Actual inputs, the local sample cursor
     * and redundancy remain intact in case another correction removes the exit. */
    bool finishReplayAt(u64 nextTick);

    u64 tick() const { return m_next; }
    u64 sampleTick() const { return m_sample; }
    u64 confirmedTick() const;
    std::optional<u64> mismatchTick() const { return m_mismatch; }
    bool replaying() const { return m_next < m_replayEnd; }
    std::span<const InputCommand> redundantInputs() const { return m_redundancy; }
    /** Up to eight retained local frames beginning at firstTick. A transport
     * whose original sends are unreliable must also resend from each receiver's
     * acknowledged input watermark: latest-eight alone can drop its missing
     * oldest frame when prediction stalls with a local input buffer. Empty means
     * the requested beginning is unavailable, not that a gap may be filled. */
    std::vector<InputCommand> resendInputs(u64 firstTick) const;

    /** Current simulated progress, separate from future input timestamps. The
     * authenticated origin must survive relaying. Stale progress never regresses
     * the watermark; advertised jumps outside the bounded window are refused.
     * transitTicks is a bounded estimate of one-way delivery age supplied by the
     * transport, not by simulation or input lead. It affects pacing only. */
    Admission observePeerTick(u8 authenticatedPeer, u64 epoch, u64 nextTick, u8 transitTicks = 0);
    /** Forward-pacing advice only: continue pumping/sending during an idle tick.
     * Replaying old ticks never waits for peer pacing. advance() enforces the
     * eight-tick prediction bound independently of this scheduling advice. */
    bool timeSyncIdle() const;
    /** Every occupied seat's actual command at a tick, once all of them are held. */
    std::optional<Commands> actualFrame(u64 tick) const;
    /** Every actual command held for any seat after a tick, in tick then seat order. */
    std::vector<InputCommand> actualAfter(u64 tick) const;
    /** Takes a trusted journal command as actual input, local seats included, outside
     * the live admission window: a journal is replayed from the first tick, not
     * predicted. Refuses a wrong epoch, grant or owner, or a value contradicting one
     * already held. A dropped peer's seat stays still from its drop on, whatever the
     * journal recorded there. */
    bool adoptActual(const InputCommand& command);
    /** After an adopted journal has been executed, local sampling continues at the tick
     * after the last local command held, never before the next tick to simulate. */
    bool resumeSampling();
    /** From `fromTick` on, a dropped peer's seats hold neutral input that counts as
     * actual: nothing is predicted, nothing is awaited and the journal records it. Its
     * input for earlier ticks is still taken, anything later is ignored. Input it sent
     * that already arrived for those ticks gives way to neutral too, and is corrected
     * where it was used, except through `settledThrough`, whose effects are past undoing.
     * The seats leave play there as well, so unsettled ticks already simulated replay. */
    bool suspend(u8 peer, u64 fromTick, u64 settledThrough = 0);
    bool suspended(u8 peer) const {
        return peer < m_suspendedFrom.size() && m_suspendedFrom[peer] != 0;
    }
    /** Whether `seat` belongs to a peer suspended at or before `tick`. */
    bool suspendedSeat(usize seat, u64 tick) const;
    /** A guest's: the host holds another guest's input through `through`. The host drops
     * a guest from the tick after its own receipt, so that guest's input, however soon a
     * direct link brought it, confirms only this far. A peer never reported bounds
     * nothing; a later report never lowers the bound. */
    bool observeHostReceipt(u8 peer, u64 through);

private:
    using Actual = std::array<std::optional<InputCommand>, InputCommand::kSeats>;
    bool occupied(u8 peer) const;
    Admission admit(u8 peer, std::span<const InputCommand> commands);
    bool complete(u64 tick, const Actual& frame) const;
    InputCommand neutral(u8 seat, u64 tick) const;
    InputCommand prediction(u8 seat, u64 tick) const;
    void trim();
    bool m_open = false;
    u8 m_local = 0;
    u8 m_delay = 0;
    u64 m_epoch = 0;
    u64 m_next = 1;
    u64 m_sample = 1;
    u64 m_actualThrough = 0;
    u64 m_replayEnd = 0;
    u64 m_replayStart = 0;
    Owners m_owners{};
    Grants m_grants{};
    std::array<u64, 5> m_peerTicks{};
    std::array<u8, 5> m_peerTransitTicks{};
    std::array<u64, 5> m_suspendedFrom{};
    std::array<std::optional<u64>, 5> m_hostReceipts{};
    Actual m_predictionSeed;
    std::map<u64, Actual> m_actual;
    std::map<u64, Frame> m_used;
    std::optional<u64> m_mismatch;
    std::vector<InputCommand> m_redundancy;
};

} // namespace gdl::game
