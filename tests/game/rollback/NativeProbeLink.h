#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>

#include "engine/net/SessionTransport.h"

#include "game/netplay/MatchSession.h"
#include "game/online/NativeRollbackDriver.h"

namespace gdl::test {
/** Test-only outbound application-datagram impairment, not an Internet RTT. */
struct NativeProbeImpairment {
    u32 latencyMs = 0;
    u32 jitterMs = 0;
    u32 lossPpm = 0;
    u32 reorderPpm = 0;
    u32 reorderMs = 0;
    u32 seed = 1;
    u32 outageTick = 0;
    u32 outageMs = 0;
    u32 outagePeer = 0;
    bool enabled() const {
        return (latencyMs != 0u) || (jitterMs != 0u) || (lossPpm != 0u) || (reorderPpm != 0u) ||
               (outageMs != 0u);
    }
};
/** Two-to-four-process iroh test driver. Local-only by default; public relay
 * diagnostics require explicit opt-in. No mocks or replica scenes. All processes use production
 * MatchSession barriers/routing and the real native rollback driver.
 * The private ticket authenticates this test link; production room admission is
 * deliberately not replaced by this fixed single-seat-per-process harness. */
class NativeProbeLink {
public:
    NativeProbeLink(bool host, const std::filesystem::path& ticket, std::string contract, u8 delay,
                    game::PlayScene& scene, u8 peers = 2, u8 localPeer = 0,
                    NativeProbeImpairment impairment = {}, bool relayOnly = false);
    NativeProbeLink(const NativeProbeLink&) = delete;
    NativeProbeLink& operator=(const NativeProbeLink&) = delete;
    ~NativeProbeLink();
    struct ConfirmedFrame {
        game::NativeRollbackDriver::ExecutedFrame frame;
        std::optional<game::NativeAgreement::Digest> digest;
    };
    struct Step {
        std::vector<ConfirmedFrame> confirmed;
        u64 simulatedThrough = 0;
        bool advanced = false;
    };
    struct Metrics {
        struct Link {
            u8 peer = 0;
            u64 samples = 0;
            u64 relaySamples = 0;
            u64 directSamples = 0;
            u64 unavailableSamples = 0;
            double rttP50Ms = 0;
            double rttP95Ms = 0;
            double rttMaxMs = 0;
        };
        struct Impairment {
            struct OutageProgress {
                u64 updates = 0;
                u64 stalls = 0;
                u64 pacingIdles = 0;
                u64 firstNextTick = 0;
                u64 lastNextTick = 0;
                u64 firstConfirmedTick = 0;
                u64 lastConfirmedTick = 0;
                u64 predictionMaxTicks = 0;
            } outageProgress;
            u64 attempted = 0;
            u64 enqueued = 0;
            u64 transmitted = 0;
            u64 lossDropped = 0;
            u64 outageDropped = 0;
            u64 reordered = 0;
            u64 oldestRecoveryDatagrams = 0;
            u64 queuePeak = 0;
            u64 queuePending = 0;
            u64 queueAbandoned = 0;
            u64 outageStartedTick = 0;
            bool outageFinished = false;
            double outageElapsedMs = 0;
            double residenceP50Ms = 0;
            double residenceP95Ms = 0;
            double residenceMaxMs = 0;
        } impairment;
        u64 sampledInputs = 0;
        double inputToUpdateP50Ms = 0;
        double inputToUpdateP95Ms = 0;
        game::NativeRollbackDriver::Statistics simulation;
        u64 predictionMaxTicks = 0;
        double correctionP50Ms = 0;
        double correctionP95Ms = 0;
        double correctionMaxMs = 0;
        std::vector<s32> linkRttMs;
        u8 localPeer = 0;
        std::vector<Link> links;
    };
    Step frame(u64 target, const std::function<game::InputCommand(u64, u8)>& sample);
    void finish();
    u64 completed() const { return m_completed; }
    Metrics metrics() const;
    game::NativeAgreement::Digest finalDigest();

private:
    class ImpairedTransport;
    using Clock = std::chrono::steady_clock;
    struct Endpoint {
        u8 peer = 0;
        bool helloSent = false;
        bool ready = false;
        bool doneSent = false;
        bool done = false;
        bool disconnected = false;
        u64 completed = 0;
    };
    void pump(bool send = true);
    void execute(bool forward, Step& step);
    void openMatch();
    void observePaths();
    bool every(bool Endpoint::*field) const;
    bool sendControl(PacketTransport::Connection connection, u8 kind);
    bool m_host;
    u8 m_peers;
    u8 m_local;
    std::string m_contract;
    std::filesystem::path m_ticket;
    std::unique_ptr<ImpairedTransport> m_wire;
    std::map<PacketTransport::Connection, Endpoint> m_endpoints;
    std::unique_ptr<game::MatchSession> m_match = std::make_unique<game::MatchSession>();
    game::PlayScene& m_scene;
    game::NativeRollbackDriver m_driver;
    u8 m_delay;
    bool m_relayOnly = false;
    bool m_invited = false;
    bool m_loaded = false;
    bool m_finishing = false;
    u64 m_completed = 0;
    u64 m_reported = 0;
    u64 m_predictionMaxTicks = 0;
    Clock::time_point m_due;
    Clock::time_point m_sendDue;
    Clock::time_point m_pumped = Clock::now();
    std::map<u64, Clock::time_point> m_samples;
    std::map<u64, game::NativeRollbackDriver::ExecutedFrame> m_frames;
    std::map<u64, game::NativeAgreement::Digest> m_digests;
    std::vector<double> m_applyMilliseconds;
    std::vector<double> m_correctionMilliseconds;
    std::vector<s32> m_rtt;
    struct PathSamples {
        u64 relayed = 0;
        u64 direct = 0;
        u64 unavailable = 0;
        std::vector<double> rtt;
    };
    std::map<u8, PathSamples> m_paths;
};
} // namespace gdl::test
