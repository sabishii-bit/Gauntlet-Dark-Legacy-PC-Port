#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "engine/net/PacketTransport.h"

namespace gdl::game {

/** Bounded agreement on an externally defined native observation, not a complete
 * state certificate. Callers must agree on the observer/content and cadence before
 * opening this stream. Never substitute a digest for an unavailable observation.
 *
 * Peer 1 compares its own digest with every occupied peer, then reliably broadcasts
 * the result. Connections and the frozen seat-to-peer roster are authenticated by
 * the caller; multiple seats on one peer require only one report. This class owns
 * no simulation, input, transport polling, or state repair. Use on one thread.
 *
 * submit() is sequential at period, 2*period, ... . Full is backpressure, not
 * permission to omit that tick. Network reports may arrive out of order within
 * the bounded window. A new epoch requires a new instance. update() uses monotonic
 * real time even if native simulation is stalled; an idle stream has no deadline.
 * After failure keep flushing the terminal notification before teardown, subject
 * to the caller's bounded transport shutdown. No failed stream can resume. */
class NativeAgreement {
public:
    using Digest = std::array<u8, 32>;
    using Owners = std::array<u8, 4>;
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Link {
        u8 peer = 0;
        PacketTransport::Connection connection = 0;
    };
    enum class Admission : u8 { Accepted, Duplicate, Stale, WrongPeer, Invalid, Full, Failed };
    enum class Failure : u8 {
        None,
        Mismatch,
        Conflict,
        Timeout,
        Window,
        Protocol,
        Disconnected,
        Transport
    };
    static constexpr u8 kVersion = 1;
    static constexpr usize kPacketBytes = 56;
    static constexpr usize kWindow = 16;
    static constexpr usize kHistory = 16;
    static constexpr usize kQueuedPackets = 16; // per destination
    static constexpr u32 kMaxPeriod = 3600;
    static constexpr auto kTimeout = std::chrono::seconds{10};
    /** A guest waits longer than the host, so the host's verdict, or its drop of whoever it
     * still waits on, always arrives first. */
    static constexpr auto kGuestTimeout = kTimeout + std::chrono::seconds{15};

    static bool recognizes(std::span<const u8> bytes);
    static bool recognizesTerminal(std::span<const u8> bytes);
    /** Validate and return the exact source epoch/tick of a terminal packet.
     * Tick zero is reserved for an epoch-wide failure, never a certificate. */
    static std::optional<std::pair<u64, u64>> terminalContext(std::span<const u8> bytes);
    /** `agreedThrough` restarts a stream at a later multiple of the period, after a
     * mismatch has been repaired, with every earlier tick taken as settled. */
    bool open(u8 localPeer, u64 epoch, const Owners& owners, std::span<const Link> links,
              u32 period = 60, Time now = Clock::now(), u64 agreedThrough = 0);
    /** One exact terminal certificate, in a distinct wire domain from periodic
     * reports even when its tick lies on the periodic schedule. */
    bool openTerminal(u8 localPeer, u64 epoch, const Owners& owners, std::span<const Link> links,
                      u64 terminalTick, Time now = Clock::now());
    Admission submit(u64 tick, const Digest& digest, Time now = Clock::now());
    Admission receive(PacketTransport::Connection connection, std::span<const u8> bytes,
                      Time now = Clock::now());
    /** False means terminal failure, or a send-discovered closed link reported to
     * closedConnection without failing the stream. The caller must resolve that
     * link after flush returns; other send errors remain terminal. */
    bool flush(PacketTransport& transport, Time now = Clock::now(),
               PacketTransport::Connection* closedConnection = nullptr);
    /** Judges the deadlines; false once the stream has failed. On peer one a tick or delivery
     * late only on a guest's account never fails it, whichever call meets the deadline:
     * overdue() names that guest for the session to drop. */
    bool update(Time now = Clock::now());
    /** On peer one: the guests a tick or a delivery is still waiting on past its deadline,
     * for the caller to drop; none when what is late is the host's own digest, which fails
     * the stream instead. */
    std::vector<u8> overdue(Time now = Clock::now()) const;
    /** Every wait starts over from `now`: a repair holds the stream, and nobody owes
     * anything until it ends. */
    void rearm(Time now = Clock::now());
    void disconnected(PacketTransport::Connection connection);
    /** A peer dropped at `now` is awaited no more and told nothing; pending ticks settle
     * without it, and what that owes the others is owed from `now`. */
    void suspend(u8 peer, Time now = Clock::now());
    bool canSubmit() const;
    bool failed() const { return m_failure != Failure::None; }
    Failure failure() const { return m_failure; }
    u64 failureTick() const { return m_failureTick; }
    /** On peer one after a mismatch: which peers' digests differed from its own. */
    const std::array<bool, 5>& mismatchedPeers() const { return m_mismatched; }
    u64 agreedTick() const { return m_agreed; }
    std::optional<u64> terminalTick() const { return m_terminal; }
    u64 submitTick() const { return m_submit; }
    usize pendingTicks() const { return m_pending.size(); }
    usize queuedPackets() const;

private:
    struct Pending {
        std::array<std::optional<Digest>, 5> digests;
        bool accepted = false;
        Time started;
    };
    struct Delivery {
        std::array<u8, kPacketBytes> bytes{};
        Time started;
    };
    std::chrono::seconds timeout() const { return m_local == 1 ? kTimeout : kGuestTimeout; }
    bool expired(Time start, Time now) const;
    bool occupied(u8 peer) const;
    bool queueReady() const;
    bool scheduled(u64 tick) const;
    bool inWindow(u64 tick) const;
    Admission admit(u8 peer, u64 tick, const Digest& digest);
    void settle();
    void fail(Failure reason, u64 tick);
    bool m_open = false;
    u8 m_local = 0;
    u64 m_epoch = 0;
    u32 m_period = 0;
    std::optional<u64> m_terminal;
    u64 m_submit = 0;
    u64 m_agreed = 0;
    Failure m_failure = Failure::None;
    u64 m_failureTick = 0;
    std::array<bool, 5> m_mismatched{};
    std::array<bool, 5> m_suspended{};
    Time m_now;
    Owners m_owners{};
    std::array<PacketTransport::Connection, 5> m_links{};
    std::array<std::deque<Delivery>, 5> m_outgoing;
    std::map<u64, Pending> m_pending;
    std::map<u64, Digest> m_history;
};
} // namespace gdl::game
