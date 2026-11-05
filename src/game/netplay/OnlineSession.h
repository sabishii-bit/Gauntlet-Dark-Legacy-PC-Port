#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>

#include "engine/net/PeerTransport.h"

#include "game/netplay/MatchSession.h"
#include "game/netplay/PartyBootstrap.h"
#include "game/netplay/RoomService.h"

namespace gdl::game {
/** Application-side room/peer/match lifecycle. The same driver runs in network
 * tests and the game; it has no HTTP, vendor, renderer or save I/O dependencies.
 * Pump on the owning thread even while loading/paused. RoomService and
 * PeerTransport must outlive this object; their destruction belongs to teardown.
 * One instance represents one room lifetime, with a frozen roster after start. */
class OnlineSession {
public:
    enum class Phase : u8 { Joining, Lobby, Starting, Active, Leaving, Closed, Failed };
    enum class Failure : u8 {
        None,
        Admission,
        Service,
        Transport,
        Protocol,
        RosterChanged,
        Timeout,
        Match
    };
    static constexpr f64 kAdmissionTimeout = 10;
    static constexpr f64 kConnectionTimeout = 20;
    static constexpr f64 kStartTimeout = 30;
    static constexpr f64 kLeaveTimeout = 5;
    // Menu sessions start a new Tower campaign with default PlayOptions. The
    // development launcher may agree another catalog scene; explicit local
    // scenario options are still compared by the exact setup digest.
    static constexpr u32 kNativeTowerScene = 14 * 256 + 1;
    static constexpr usize kNativePacketBytes = 1024;
    struct NativeIdentity {
        std::string build;   ///< the game version; the sources themselves are in runtime
        std::string runtime; ///< NativeOnlineIdentity: the sources built and the numeric witness
        std::string content; ///< SHA-256 of the native asset tree
        std::string data;    ///< SHA-256 of the resolved gameplay/text data tree
        std::string language;
        bool valid() const;
        bool operator==(const NativeIdentity&) const = default;
    };
    struct NativeRules {
        u32 scene = kNativeTowerScene;
        bool autoMelee = false;
        bool autoActivateItems = true;
        bool expandedShop = false;
        bool autoAim = true;
        u8 startupDelay = 1;
        bool valid() const;
        bool operator==(const NativeRules&) const = default;
    };
    struct NativeContract {
        NativeIdentity identity;
        NativeRules rules;
        bool operator==(const NativeContract&) const = default;
    };

    OnlineSession(RoomService& service, PeerTransport& transport, bool host, u8 localPlayers);
    ~OnlineSession();
    OnlineSession(const OnlineSession&) = delete;
    OnlineSession& operator=(const OnlineSession&) = delete;
    OnlineSession(OnlineSession&&) = delete;
    OnlineSession& operator=(OnlineSession&&) = delete;

    void update(f64 seconds);
    /** Send newly sampled input after gameplay, without another poll,
     * simulation step or clock advance. Congestion retains pending output. */
    bool flush();
    /** Local-device order, mapped to the room's assigned seats on start. Select
     * before ready; unready before changing an existing lobby selection. */
    bool select(std::span<const CharacterProfile> selections);
    bool ready(bool value = true);
    bool start();
    /** Required before readiness. Only the host supplies rules; guests verify
     * their local content/runtime identity against its offer. No match opens
     * until every peer confirms the complete resolved native setup. */
    bool configureNative(const NativeIdentity& local,
                         std::optional<NativeRules> hostRules = std::nullopt);
    bool nativeRequested() const { return m_native != nullptr; }
    /** Available while Starting, before Active/openMatch. The application builds
     * and retains its canonical setup from this contract + party(), then confirms
     * that setup's exact digest. This class never loads scenes or reads config. */
    const NativeContract* nativeContract() const;
    bool nativeSetupPending() const;
    bool confirmNativeSetup(const std::string& setup);
    /** Empty until the exact setup has completed the native bootstrap agreement. */
    std::string_view confirmedNativeSetup() const;
    void leave();
    Phase phase() const { return m_phase; }
    Failure failure() const { return m_failure; }
    const std::string& failureDetail() const { return m_failureDetail; }
    bool host() const { return m_host; }
    bool connected() const;
    /** A guest's links straight to other guests, which carry their inputs sooner. */
    usize directLinks() const;
    /** The host's startup input delay for the match about to start, chosen from the
     * lobby's measured links; refused once the contract carrying it exists, on a guest,
     * or beyond the rollback's longest delay. */
    bool adoptStartupDelay(u8 delay);
    /** The slowest live link's transport figures: the highest ping, whether any link
     * is relayed, and all pending bytes; nothing before a link exists. */
    std::optional<PacketTransport::Statistics> linkStatistics() const;
    const RoomSnapshot* room() const { return m_room ? &*m_room : nullptr; }
    std::span<const u8> seats() const { return m_seats; }
    const PartyBootstrap::Party* party() const {
        return m_party.complete() ? &m_party.party() : nullptr;
    }
    MatchSession& match() { return m_match; }
    const MatchSession& match() const { return m_match; }

private:
    using Connection = PacketTransport::Connection;
    struct ReadyRequest {
        u64 revision;
        bool value;
    };
    bool admit(RoomSnapshot room, const std::string& endpointId);
    /** Accepts a started room that only lost members, handling each as its lost link
     * would be: the host drops it, a guest forgets its direct link. False for any other
     * change to a started room, which stays a conflict. */
    bool departed(const RoomSnapshot& next);
    void event(const PacketTransport::Event& event);
    void openMatch();
    bool nativeMessage(Connection connection, std::span<const u8> bytes);
    bool advanceNativeBootstrap();
    bool flushNativeBootstrap();
    void fail(Failure reason);
    void closeLinks();
    std::vector<std::string> allowed() const;
    std::vector<std::string> required() const;

    RoomService& m_service;
    PeerTransport& m_transport;
    bool m_host;
    u8 m_localPlayers;
    Phase m_phase = Phase::Joining;
    Failure m_failure = Failure::None;
    std::string m_failureDetail;
    std::optional<RoomSnapshot> m_room;
    std::string m_endpointId;
    std::vector<u8> m_seats;
    std::map<std::string, Connection> m_links;
    std::optional<Connection> m_connecting;
    std::map<std::string, Connection> m_dialing; ///< direct links not yet up
    std::optional<ReadyRequest> m_ready;
    std::optional<PacketTransport::Event> m_prepare;
    std::vector<PacketTransport::Event> m_earlyParty;
    std::vector<CharacterProfile> m_selections;
    PartyBootstrap m_party;
    struct NativeBootstrap;
    std::unique_ptr<NativeBootstrap> m_native;
    f64 m_wait = 0;
    MatchSession m_match;
};
} // namespace gdl::game
