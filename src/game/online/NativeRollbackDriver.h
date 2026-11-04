#pragma once

#include <memory>
#include <vector>

#include "engine/io/NativeStateDigest.h"

#include "game/netplay/MatchSession.h"
#include "game/rollback/InputJournal.h"
#include "game/rollback/RollbackInputs.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** In-process predicted native simulation for one frozen epoch. Transport,
 * sampling cadence, load barriers and physical output dispatch are callers'
 * responsibilities. MatchSession transports input commands, not scene state.
 *
 * Recorded audio is an explicit opt-in: the caller attaches the scene recorder
 * before binding and dispatches its corrected delta after advance. Unsupported
 * capture paths fail closed. Confirmation hashes use the retained post-tick
 * native scene plus the driver's actual rewindable control state. */
class NativeRollbackDriver {
public:
    using Ledger = PlayScene::OutputLedger;
    using Admission = RollbackInputs::Admission;
    enum class AudioMode : u8 { Disabled, Required };
    enum class Status : u8 { Held, Advanced, Travel, GameOver, Leave, Failed };
    enum class Failure : u8 { None, Binding, Input, Snapshot, Replay, Journal, Audio, Observation };
    static constexpr u64 kDigestPeriod = 30;
    struct ExecutedFrame {
        u64 tick = 0;
        RollbackInputs::Frame input;
        Ledger::Journal journal;
        bool replay = false;
        std::optional<SceneSoundEvents::Frame> audio;
    };
    struct ConfirmedDigest {
        u64 tick = 0;
        NativeStateDigest::Digest digest;
        bool operator==(const ConfirmedDigest&) const = default;
    };
    struct Result {
        Status status = Status::Held;
        /** Bounded to eight corrected frames plus at most one forward frame. */
        std::vector<ExecutedFrame> frames;
        std::vector<Ledger::Action> actions;
        std::vector<FrameEventIdentity> unretractable;
        std::vector<ConfirmedDigest> digests;
        std::vector<PlayScene::ConsoleReply> consoleReplies; ///< confirmed, exactly once
        /** Exact confirmed final frame, including off-cadence exits. Published
         * once alongside Travel/GameOver/Leave; periodic digests stay on their
         * ordinary cadence and never contain a duplicate terminal entry. */
        std::optional<ConfirmedDigest> terminalDigest;
        /** Present only in Required mode after successful publication. No
         * intermediate replay frame is dispatched to the audio device. */
        std::optional<SceneSoundEvents::Result> audio;
    };
    struct Statistics {
        u64 nativeTicks = 0;
        u64 replayTicks = 0;
        u64 corrections = 0;
        u64 maxReplay = 0;
        u64 stalls = 0;
        u64 pacingIdles = 0;
    };
    struct Terminal {
        u64 tick = 0;
        PlayOutcome outcome = PlayOutcome::Running;
        bool operator==(const Terminal&) const = default;
    };
    /** Actual rewindable coordinator state, separate from local input sampling,
     * transport progress and presentation deduplication. */
    struct Control {
        std::array<bool, InputCommand::kSeats> arrivalVotes{};
        std::optional<Terminal> terminal;
        bool operator==(const Control&) const = default;
    };
    /** Common schema for a captured native scene and its same-tick control.
     * Offline input-reference harnesses use this with their actual control. */
    static NativeStateDigest::Digest agreementDigest(const NativeStateDigest::Digest& scene,
                                                     const Control& control);

    NativeRollbackDriver();
    ~NativeRollbackDriver();
    NativeRollbackDriver(const NativeRollbackDriver&) = delete;
    NativeRollbackDriver& operator=(const NativeRollbackDriver&) = delete;
    /** Requires the actual loaded party, 60 Hz and negotiated local delay 0..4.
     * A new binding requires a newer epoch, including after clear(). A failed
     * loaded scene cannot bind again until it has been closed and reopened.
     * Old scene resources must outlive clear().
     * Required audio must already be recording this epoch, before its first tick.
     * Clear the driver before detaching the scene recorder or closing its banks. */
    bool bind(PlayScene& scene, const MatchContext& context, u8 localPeer,
              AudioMode audio = AudioMode::Disabled);
    /** Release histories and borrowed resources, retaining the epoch high-water
     * mark and failed load identities. clear() does not repair a partial restore. */
    void clear();
    /** Local device order maps to owned seats in ascending order. Accumulates
     * unsampled press edges under backpressure; admitted frames never change.
     * No sampling, semantic pointer lookup or device polling occurs on replay. */
    bool sample(const SessionInputs::Frame& local,
                std::optional<ConsoleAction> console = std::nullopt);
    /** Cancel only a local seat's unsampled controls when its local menu opens.
     * Already admitted input history remains immutable. A valid owned seat with
     * no pending capture succeeds; foreign or unbound seats are rejected. */
    bool clearPendingInput(u8 canonicalSeat);
    Admission submit(u8 authenticatedPeer, std::span<const InputCommand> commands);
    Admission observePeerTick(u8 authenticatedPeer, u64 epoch, u64 nextTick, u8 transitTicks = 0);
    /** Reconcile all corrections, optionally advance one new tick, then publish
     * outputs once. forward=false still corrects and confirms a held scene.
     * Dispatch successful actions exactly once after this method returns.
     * Rumble/unlock callbacks never run here; Required mode also isolates audio.
     * Disabled mode retains the scene's direct offline audio behavior.
     * Terminal results are confirmed only. */
    Result advance(bool forward = true);

    bool sampleReady() const;
    u64 sampleTick() const;
    u64 nextTick() const;
    u64 confirmedTick() const;
    std::optional<u64> mismatchTick() const;
    std::optional<Terminal> tentativeTerminal() const;
    std::array<bool, InputCommand::kSeats> arrivalVotes() const;
    std::span<const InputCommand> redundantInputs() const;
    const Statistics& statistics() const;
    /** The epoch's confirmed input record so far. */
    const EpochInputJournal& journal() const;
    /** Actual input held beyond a tick for any seat: what a rebuilding peer needs after
     * the journal so that it never contradicts input it already sent. */
    std::vector<InputCommand> actualInputsAfter(u64 tick) const;
    /** Binds the bound epoch again, to a freshly loaded scene or to one restoreBinding()
     * returned to the epoch's start, for a peer rebuilding the stage from a journal;
     * otherwise as bind(). */
    bool rebind(PlayScene& scene, const MatchContext& context, u8 localPeer,
                AudioMode audio = AudioMode::Disabled);
    /** Returns the bound scene to the state it was bound in and releases the histories, for
     * a stage that no load can bring back to its epoch's start; rebind() follows. */
    bool restoreBinding();
    /** Takes trusted journal commands as actual input for any seat, local ones included. */
    bool adopt(std::span<const InputCommand> commands);
    /** Replays adopted input to `through` from the first tick without pacing, per-tick
     * history or digests before it. Call before advancing; finishRebuild() once the head
     * has passed `through` resumes local sampling where the journal left off. */
    bool beginRebuild(u64 through);
    /** A dropped peer's seats hold neutral input from `fromTick` on (see RollbackInputs), and
     * from that tick on its characters wait in the tower (PlayScene::waitInTower). */
    bool suspend(u8 peer, u64 fromTick);
    /** A guest's: how far the host holds another guest's input; see RollbackInputs. */
    bool observeHostReceipt(u8 peer, u64 through);
    bool finishRebuild();
    bool rebuilding() const;
    Failure failure() const;

private:
    struct Load;
    struct Data;
    std::unique_ptr<Data> m_data;
    std::vector<std::shared_ptr<Load>> m_loads;
    u64 m_latestEpoch = 0;
    bool m_rebinding = false;
};
} // namespace gdl::game
