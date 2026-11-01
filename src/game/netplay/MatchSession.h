#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <string>

#include "game/netplay/NativeAgreement.h"
#include "game/netplay/RollbackExchange.h"
#include "game/netplay/ShopSync.h"
#include "game/rollback/InputJournal.h"
#include "game/rollback/PartyCheckpoint.h"

namespace gdl::game {

/** Frozen, room-admitted roster. Peer 1 is the host; these IDs are not socket handles. */
using MatchOwners = std::array<u8, InputCommand::kSeats>;
struct MatchLink {
    u8 peer = 0;
    PacketTransport::Connection connection = 0;
};
// Resume and the pause controls retain their old numeric values only so malformed
// obsolete requests can be rejected explicitly. Local menus never pause the shared clock.
enum class MatchTransition : u8 { Start, Travel, Resume, Shop, SecretEnter, SecretReturn };
enum class MatchStop : u8 { None, Left, Disconnected, LoadTimeout, TransportFailure };
enum class MatchControlKind : u8 {
    Prepare,
    Ready,
    Commit,
    Pause,
    RequestPause,
    Stop,
    MovieSkipVote,
    Resync,     ///< host: the named peers rebuild from the journal at the given tick
    ResyncDone, ///< a rebuilt peer to the host, and the host on to everyone
    Drop        ///< host: the named peer is out; its seats hold still from the given tick
};
struct MatchContext {
    u64 epoch = 0;
    u32 scene = 0; // trusted build's scene roster, never a peer-supplied path
    u16 tickRate = 60;
    u8 inputLead = 1; // fixed local input buffer, negotiated in the range 0..2
    MatchTransition transition = MatchTransition::Start;
    MatchOwners owners{};
    std::array<u32, InputCommand::kSeats> grants{};
    bool challengeWon = false;
    // Travel adopts the next context only after the actual source scene executes
    // this terminal boundary. Both are zero on the initial Start context.
    u64 sourceEpoch = 0;
    u64 sourceTick = 0;
    bool valid() const;
    bool operator==(const MatchContext&) const = default;
};
struct MatchControl {
    MatchControlKind kind = MatchControlKind::Prepare;
    MatchStop stop = MatchStop::None;
    MatchContext context;
    u8 movieSkipVotes = 0; // occupied seat bits, never machine bits
    u64 resyncTick = 0;    // Resync: the tick agreement restarts at; Drop: the first still tick
    u8 resyncPeers = 0;    // Resync/ResyncDone/Drop: bit per peer id
    bool valid() const;
};
class MatchControlPacket {
public:
    static constexpr u8 kVersion = 8;
    static constexpr usize kBytes = 72;
    static std::optional<std::vector<u8>> encode(const MatchControl& control);
    static std::optional<MatchControl> decode(std::span<const u8> bytes);
};

/** A rebuild the host ordered for the peers named by bit from the confirmed tick: every
 * peer restarts digest agreement there and the named peers reload the stage and replay the
 * journal the host sends them, while everyone else waits. */
struct MatchResync {
    u64 tick = 0;
    u8 peers = 0;
    bool operator==(const MatchResync&) const = default;
};
/** Native shared-simulation lifecycle and authenticated rollback input routing.
 * Every peer owns its simulation/history; this owner transports only controls,
 * actual inputs and confirmed digests. No snapshots, replicated audio, host
 * migration, mid-match joins or save writes belong here.
 * Load, movie, shop and source-terminal barriers retain reliable messages under
 * congestion; wall time can time out a peer but never advances gameplay. */
class MatchSession {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Phase : u8 { Offline, Lobby, Loading, Running, Stopped };
    enum class Admission : u8 { Accepted, Stale, WrongPeer, WrongPhase, Invalid };
    static constexpr f64 kLoadTimeout = 120;
    static constexpr usize kControlBudget = 16;
    static constexpr u32 kNativeAgreementPeriod = 30;

    /** `direct` are a guest's links to other guests: their inputs arrive on them sooner
     * than through the host, which still relays everything and decides everything. */
    bool open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links,
              u8 startupDelay = 1, std::span<const MatchLink> direct = {});
    /** A new transport/room lifetime is required before opening again after clear(). */
    void clear();
    /** Checkpointed transitions require a complete host party; Shop also its setup.
     * Invalid or congested preparation leaves the current epoch unchanged. */
    bool prepare(u32 scene, MatchTransition transition = MatchTransition::Start,
                 const MatchParty* party = nullptr, const ShopSetup* shop = nullptr,
                 bool challengeWon = false);
    ShopSync& shop() { return m_shop; }
    const ShopSync& shop() const { return m_shop; }
    const MatchParty* travelParty() const;
    bool loaded();
    bool requestMovieSkip(u8 seat);
    /** Room policy, host-only and off by default. Revocation cancels queued guest
     * requests, never input commands already admitted to the timeline. */
    bool setConsolePermissions(bool enabled);
    bool consolePermissions() const { return m_consolePermissions; }
    /** Reliable request for the calling peer's seat. The host consumes accepted
     * requests into its own next immutable input, never executes them on receipt. */
    bool requestConsole(const ConsoleAction& action);
    std::optional<ConsoleAction> takeConsoleRequest();
    bool movieSkipReady() const;
    u8 movieSkipVotes() const { return m_movieSkipVotes; }
    /** Global pause has no shared tick boundary and is deliberately unsupported. */
    static bool requestPause() { return false; }
    void leave();
    /** seconds is lifecycle elapsed time; now is independent monotonic watchdog time. */
    void update(f64 seconds, Time now = Clock::now());
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes,
                      std::optional<Time> now = std::nullopt, s32 pingMs = -1);
    void flush(PacketTransport& transport, Time now = Clock::now());

    using NativeInputConsumer =
        std::function<bool(const RollbackExchange::Message&, std::optional<u8>)>;
    /** Bind the loaded epoch's native timeline before Ready. Admission is
     * synchronous and must validate both inputs and fresh peer progress; any
     * refusal stops the session without publishing an ACK. Detach before
     * releasing that timeline. Source-epoch relay never calls a detached owner. */
    bool attachNativeInputs(u64 epoch, NativeInputConsumer consumer);
    void detachNativeInputs(u64 epoch);
    /** Publish after sampling and after execution/correction. nextTick is the
     * actual current head, not the input sample cursor or received watermark. */
    bool publishNativeInputs(std::span<const InputCommand> commands, u64 nextTick);
    /** The caller supplies only executed, all-seat-confirmed input. Receipt
     * alone cannot advance this watermark or acknowledge a transition. */
    bool confirmNativeTick(u64 epoch, u64 through);
    bool observeNativeTerminal(u64 epoch, u64 tick, const NativeAgreement::Digest& digest,
                               Time now = Clock::now());
    bool nativeTerminalAgreed(u64 epoch, u64 tick) const;
    /** Mark a confirmed terminal after validating the native outcome/party. If
     * Prepare is pending, this separately adopts it; repeat when a late Prepare
     * arrives. The host must mark its terminal before preparing ordinary travel. */
    bool completeNativeBoundary(u64 epoch, u64 tick);
    /** Buffered confirmed hashes may precede the current confirmed watermark.
     * The agreed native observer/cadence defines their schema, not this stream. */
    bool observeNative(u64 tick, const NativeAgreement::Digest& digest, Time now = Clock::now());
    const NativeAgreement* nativeAgreement() const { return m_nativeAgreement.get(); }
    bool nativeDraining() const { return m_pendingNativeContext.has_value(); }
    const MatchContext* pendingNativeContext() const {
        return m_pendingNativeContext ? &*m_pendingNativeContext : nullptr;
    }
    std::optional<u64> nativeEndTick() const {
        return m_pendingNativeContext ? std::optional{m_pendingNativeContext->sourceTick}
                                      : std::nullopt;
    }
    u64 nativeHead() const { return m_nativeNextTick; }
    u64 nativeConfirmed() const { return m_nativeCompleted; }
    bool nativeInputs() const { return m_phase != Phase::Offline; }
    bool host() const { return m_local == 1; }
    u8 localPeer() const { return m_local; }
    const MatchContext& context() const { return m_context; }
    Phase phase() const { return m_phase; }
    MatchStop stopReason() const { return m_stop; }
    /** Local diagnostic, never peer-supplied text or an invitation/room code. */
    const std::string& failureDetail() const { return m_failureDetail; }
    u8 inputLead() const { return m_startupDelay; }

    using Resync = MatchResync;
    static constexpr usize kJournalDeliveries = 1 << 16;
    static constexpr usize kJournalCommands = 1 << 21;
    static constexpr usize kRecentDigests = 16;
    bool resyncing() const { return m_resync.has_value(); }
    const Resync* resync() const { return m_resync ? &*m_resync : nullptr; }
    /** The host's: the peers that still owe a rebuild and have no journal queued yet. */
    std::optional<Resync> pendingResync() const;
    /** Queues one journal chunk for a peer that owes a rebuild. */
    bool queueJournal(u8 peer, const JournalPacket::Chunk& chunk);
    bool journalQueued(u8 peer);
    usize queuedJournal() const { return m_journalDeliveries.size(); }
    /** A named peer's: the rebuild it was ordered to do, its journal once whole, and
     * its completion, which tells the host and from there everyone. */
    std::optional<u64> resyncRequested() const;
    bool journalReady() const { return m_resyncTarget && m_journalComplete; }
    std::vector<InputCommand> takeJournal();
    bool finishResync();

    /** A peer the host has dropped: its seats hold neutral input from `tick` of `epoch`
     * on, and from the first tick of every later epoch; its characters stay in the party.
     * In a shop `tick` is one past the host's next step, the first its lane is gone from. */
    struct Drop {
        u8 peer = 0;
        u64 epoch = 0;
        u64 tick = 0;
        bool operator==(const Drop&) const = default;
    };
    /** The host's: drops a guest from the match and tells everyone, the guest included
     * when still reachable. The first still tick is the one after its last input seen. */
    bool drop(u8 peer);
    bool dropped(u8 peer) const { return peer < m_dropped.size() && m_dropped[peer].peer != 0; }
    /** Every drop that holds for `epoch`, with the tick its driver holds the seats still from. */
    std::vector<Drop> dropsFor(u64 epoch) const;
    /** Whether a dropped `peer`'s lane is gone by shop step `step` of `epoch`, so that every
     * peer ends it with the step the host ended it at, whenever the drop reached it. */
    bool shopLaneGone(u8 peer, u64 epoch, u64 step) const;
    /** Drops not yet applied to the running driver, each taken once. */
    std::vector<Drop> takeDrops();
    /** Peers whose input lags the local head by more than the prediction window: the
     * ones a stall is waiting on. */
    std::vector<u8> quietPeers() const;
    /** A guest's: how far the host holds a guest's input that a direct link may bring
     * sooner; pass it to the driver before advancing (RollbackInputs::observeHostReceipt). */
    std::optional<u64> hostReceipt(u8 peer) const {
        return m_exchange ? m_exchange->hostReceipt(peer) : std::nullopt;
    }
    /** Passes every hostReceipt to `inputs` (a driver or RollbackInputs), as each advance
     * needs first; false when it refuses one. */
    template <class Inputs> bool reportHostReceipts(Inputs& inputs) const {
        for (u8 peer = 2; peer <= 4; ++peer) {
            const auto held = hostReceipt(peer);
            if (held && !inputs.observeHostReceipt(peer, *held)) {
                return false;
            }
        }
        return true;
    }
    /** False when the lost link ends the match; true when the peer was dropped instead. */
    bool disconnected(PacketTransport::Connection connection);
    /** A direct link closed; the host's relay carries that peer from here. */
    void directLost(PacketTransport::Connection connection);
    usize directLinks() const;
    usize queuedControls() const { return m_controls.size(); }

private:
    struct ControlDelivery {
        PacketTransport::Connection connection;
        std::vector<u8> bytes;
    };
    u8 peerOf(PacketTransport::Connection connection) const;
    bool queue(u8 peer, MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0,
               Resync resync = {});
    bool broadcast(MatchControlKind kind, MatchStop reason = MatchStop::None, u8 votes = 0,
                   Resync resync = {});
    bool beginResync(Time now);
    bool restartAgreement(u64 tick, Time now);
    bool agreementFailed(Time now);
    /** The roster without dropped peers, for streams opened after a drop. */
    MatchOwners activeOwners() const;
    u8 directPeerOf(PacketTransport::Connection connection) const;
    Admission nativeInputs(PacketTransport::Connection connection, std::span<const u8> bytes,
                           s32 pingMs);
    void applyDrop(const Drop& drop);
    Admission journal(u8 peer, const JournalPacket::Chunk& chunk);
    void tryCommit();
    void stop(MatchStop reason, bool notify);
    void discardGameplay();
    bool beginNativeInputs();
    bool ensureNativeAgreement(Time now);
    bool ensureTerminalAgreement(u64 tick, Time now);
    void failNativeAgreement(NativeAgreement::Failure fallback, u64 tick = 0,
                             const NativeAgreement* source = nullptr);
    bool acceptPreparedContext(const MatchContext& context, MatchParty party = {});
    bool finishNativeDrain();
    Admission control(u8 peer, const MatchControl& message);
    Admission checkpoint(u8 peer, const PartyCheckpoint& message);
    bool consoleReady() const;
    Admission consoleMessage(u8 peer, std::span<const u8> bytes);
    bool admitConsole(u8 peer, u64 epoch, u64 revision, u64 request, u32 grant,
                      const ConsoleAction& action);

    std::string m_failureDetail;
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<PacketTransport::Connection, 5> m_droppedLinks{}; ///< what a drop took down
    std::array<PacketTransport::Connection, 5> m_direct{};
    MatchContext m_context;
    std::unique_ptr<RollbackExchange> m_exchange;
    NativeInputConsumer m_nativeConsumer;
    std::unique_ptr<NativeAgreement> m_nativeAgreement;
    std::unique_ptr<NativeAgreement> m_terminalAgreement;
    u64 m_nativeNextTick = 0;
    u64 m_nativeCompleted = 0;
    std::optional<u64> m_nativeTerminal;
    std::optional<MatchContext> m_pendingNativeContext;
    MatchParty m_pendingNativeParty;
    std::unique_ptr<ShopSync> m_pendingNativeShop;
    // Only the old relay survives loading, not another scene. Ready acknowledges
    // execution through its fence; tryCommit retires this relay.
    std::unique_ptr<RollbackExchange> m_drainingInputs;
    std::unique_ptr<NativeAgreement> m_drainingAgreement;
    std::unique_ptr<NativeAgreement> m_drainingTerminal;
    std::optional<MatchContext> m_drainingContext;
    MatchParty m_travelParty;
    ShopSync m_shop;
    f64 m_loadingSeconds = 0;
    std::deque<ControlDelivery> m_controls;
    struct ConsoleRequest {
        u8 peer = 0;
        u64 epoch = 0;
        ConsoleAction action;
    };
    static constexpr usize kConsoleRequests = 32;
    u64 m_consoleRevision = 0;
    u64 m_consoleNextRequest = 0;
    std::array<u64, 5> m_consoleSeen{};
    std::deque<ConsoleRequest> m_consoleRequests;
    std::deque<ControlDelivery> m_journalDeliveries;
    std::map<u64, NativeAgreement::Digest> m_recentDigests;
    std::optional<Resync> m_resync;
    std::vector<InputCommand> m_journalIncoming;
    std::array<Drop, 5> m_dropped{};
    std::vector<Drop> m_pendingDrops;

    // Keep byte-sized lifecycle state together rather than between aligned streams.
    Phase m_phase = Phase::Offline;
    MatchStop m_stop = MatchStop::None;
    u8 m_local = 0;
    MatchOwners m_owners{};
    std::array<bool, 5> m_loaded{};
    u8 m_movieSkipVotes = 0;
    u8 m_movieSkipRequests = 0;
    u8 m_startupDelay = 1;
    u8 m_pendingNativeMovieVotes = 0;
    bool m_consolePermissions = false;
    u8 m_journalQueuedPeers = 0;
    bool m_resyncTarget = false;
    bool m_journalComplete = false;
};
} // namespace gdl::game
