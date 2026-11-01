#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>

#include "engine/net/PacketTransport.h"

#include "game/rollback/RollbackInputs.h"

namespace gdl::game {

/** Transport-only input delivery for an independently simulated epoch. Peer one
 * routes authenticated origins; it does not wait for or authorize simulation.
 * Progress and receiver-by-origin ACKs survive routing through that peer.
 * Call flush on a bounded network cadence (normally 60 Hz), even while prediction
 * is stalled, not on every unlimited-rate render. A lost oldest input must remain
 * recoverable after it has left the latest-eight redundancy window.
 *
 * MatchSession owns this exchange; each peer's native driver owns simulation.
 * No gameplay state, device sampling, save data, or provider is owned here. */
class RollbackExchange {
public:
    using Owners = RollbackInputs::Owners;
    using Grants = RollbackInputs::Grants;
    using Watermarks = std::array<u64, 5>; // peer IDs 1..4; index zero is reserved
    using Clock = std::function<u64()>;    // local monotonic microseconds, never sent
    struct Link {
        u8 peer = 0;
        PacketTransport::Connection connection = 0;
    };
    struct Message {
        u64 epoch = 0;
        u8 origin = 0;
        u64 nextTick = 1;
        Watermarks received{};
        std::vector<InputCommand> commands;
        u64 progressSerial = 1;
        u32 progressAgeUs = 0; // age at send, including upstream transit/residence
    };
    enum class Admission : u8 {
        Accepted,
        WrongPeer,
        WrongEpoch,
        Invalid,
        Conflict,
        Full,
        Rejected
    };
    enum class Failure : u8 { None, Conflict, Window, Disconnected, Transport };
    // The room adds one byte; keep inputs in one 1000-byte provider fragment.
    // Variable console text splits earlier, between complete owned-seat frames.
    static constexpr usize kHeaderBytes = 68;
    static constexpr usize kMaxBytes = 999;
    static constexpr usize kHistory = 128;
    static constexpr u8 kVersion = 2;
    static constexpr u32 kMaxProgressAgeUs =
        static_cast<u32>((RollbackInputs::kPredictionTicks * 1'000'000 + 59) / 60);
    static std::optional<std::vector<u8>> encode(const Message& message);
    static std::optional<Message> decode(std::span<const u8> bytes);
    static bool recognizes(std::span<const u8> bytes);

    /** Links: the host's to every guest, a guest's to the host, and optionally a guest's
     * direct links to other guests, which carry their own inputs sooner than the relay.
     * `absent` are peers dropped before this epoch: they need no link and start suspended. */
    bool open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants, u8 delay,
              std::span<const Link> links, Clock clock = {}, std::span<const u8> absent = {});
    /** After successful device sampling, pass retained local inputs and current
     * progress. An empty span still publishes progress/ACKs during a held frame. */
    bool publish(std::span<const InputCommand> commands, u64 nextTick);
    /** consume must synchronously admit commands/progress to the native driver.
     * A refusal publishes no ACK or forwarded input. Unexpected driver conflicts
     * must stop the session, not silently substitute input. Never retain Message&.
     * pingMs is this incoming connection's measured RTT, not an origin's RTT.
     * Negative/unknown RTT uses the conservative eight-tick pacing allowance.
     * The optional callback age is present only for fresh origin progress;
     * duplicate/reordered reports may still contain useful inputs and ACKs. */
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes, s32 pingMs,
                      const std::function<bool(const Message&, std::optional<u8>)>& consume);
    /** An optional closed-link result lets the owning session drop a guest before
     * marking the exchange failed. Handle it only after flush returns: dropping a
     * peer can retire this exchange while completing a stage transition. */
    bool flush(PacketTransport& transport,
               PacketTransport::Connection* closedConnection = nullptr);
    void disconnected(PacketTransport::Connection connection);
    /** Forgets a direct link that closed without failing the stream: the host's relay
     * still carries that peer's inputs. */
    void detach(PacketTransport::Connection connection);
    /** Stops sending to a dropped peer and waiting on its acknowledgements; its own
     * earlier inputs are still resent to the others. */
    void suspend(u8 peer);
    Failure failure() const { return m_failure; }
    u64 receivedThrough(u8 origin) const;
    u64 acknowledgedThrough(u8 receiver, u8 origin) const;
    usize retainedFrames(u8 origin) const;
    /** A guest's: how far the host holds the input of a guest this peer was opened with a
     * direct link to, by the host's own acknowledgements, even once that link is gone.
     * Nothing for an origin heard only through the host, which holds all it relays. */
    std::optional<u64> hostReceipt(u8 origin) const;

private:
    using Frame = std::vector<InputCommand>;
    using History = std::map<u64, Frame>;
    struct Progress {
        u64 serial = 0;
        u32 ageUs = 0;
        u64 observedAt = 0; // this machine's clock only
    };
    bool occupied(u8 peer) const;
    Admission validate(const Message& message, History& incoming) const;
    void commit(const Message& message, const History& incoming, std::optional<Progress> progress);
    std::optional<u64> now();
    void trim();
    bool fail(Failure reason);
    bool m_open = false;
    u8 m_local = 0;
    u8 m_delay = 0;
    u64 m_epoch = 0;
    Owners m_owners{};
    Grants m_grants{};
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<History, 5> m_history;
    std::array<Watermarks, 5> m_received{};
    Watermarks m_next{};
    std::array<Progress, 5> m_progress;
    Clock m_clock;
    u64 m_lastTime = 0;
    std::array<u8, 5> m_sendOrigin{1, 1, 1, 1, 1};
    std::array<bool, 5> m_suspended{};
    std::array<bool, 5> m_direct{}; ///< a guest's origins it was opened with a direct link to
    Failure m_failure = Failure::None;
};
} // namespace gdl::game
