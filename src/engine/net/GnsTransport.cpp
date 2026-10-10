#include "engine/net/GnsTransport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <utility>

#include <google/protobuf/unknown_field_set.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>

#include "engine/core/Log.h"

namespace gdl {
namespace {

std::atomic_bool& runtimeOwned() {
    static std::atomic_bool owned{false};
    return owned;
}

bool validPeer(const std::string& peer) {
    return peer.size() == 32 && std::ranges::all_of(peer, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

SteamNetworkingIdentity networkIdentity(const std::string& peer) {
    SteamNetworkingIdentity identity{};
    // Generic strings allow only 31 bytes plus the terminator. Generic bytes
    // preserve our full 32-character room identity, without silent truncation.
    if (!identity.SetGenericBytes(peer.data(), peer.size())) {
        identity.Clear();
    }
    return identity;
}

bool signalIdentitiesMatch(std::span<const u8> bytes, const std::string& sender,
                           const std::string& recipient) {
    // GNS 1.6's CMsgSteamNetworkingP2PRendezvous: from_identity=8, to_identity=10.
    // Check the authenticated service envelope against the opaque payload BEFORE
    // GNS dispatches it: its receive context runs only for new connections, not
    // existing ones. Otherwise one admitted guest could spoof another guest's
    // cleanup/handshake signals. Protobuf handles wire lengths/types/recursion;
    // all other fields remain opaque to us. Recheck this contract on GNS upgrades.
    google::protobuf::UnknownFieldSet message;
    if (!message.ParseFromArray(bytes.data(), static_cast<s32>(bytes.size()))) {
        return false;
    }
    std::array<char, SteamNetworkingIdentity::k_cchMaxString> from{};
    std::array<char, SteamNetworkingIdentity::k_cchMaxString> to{};
    networkIdentity(sender).ToString(from.data(), static_cast<s32>(from.size()));
    networkIdentity(recipient).ToString(to.data(), static_cast<s32>(to.size()));
    bool fromSeen = false;
    bool toSeen = false;
    for (s32 index = 0; index < message.field_count(); ++index) {
        const auto& field = message.field(index);
        if (field.number() != 8 && field.number() != 10) {
            continue;
        }
        if (field.type() != google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
            return false;
        }
        auto& seen = field.number() == 8 ? fromSeen : toSeen;
        const auto* expected = field.number() == 8 ? from.data() : to.data();
        if (seen || field.length_delimited() != expected) {
            return false;
        }
        seen = true;
    }
    return fromSeen && toSeen;
}

} // namespace

struct GnsTransport::Impl {
    struct Link {
        Connection id;
        bool connected = false;
        std::string peer;
    };
    // GNS may call these on a worker thread. The owner retains the small adapter
    // until Release has completed; shutdown destroys GNS before these objects.
    struct Signaling final : ISteamNetworkingConnectionSignaling {
        Impl& owner;
        const std::string remote;
        std::atomic_bool released{false};
        Signaling(Impl& impl, std::string peer) : owner(impl), remote(std::move(peer)) {}
        bool SendSignal(HSteamNetConnection /*unused*/, const SteamNetConnectionInfo_t& /*unused*/,
                        const void* data, int size) override {
            if (size <= 0 || static_cast<usize>(size) > kMaxSignalBytes) {
                return false;
            }
            const std::span bytes(static_cast<const u8*>(data), static_cast<usize>(size));
            if (owner.trace) {
                log::info("Network signal queued: {} bytes", size);
            }
            const std::scoped_lock lock(owner.signalMutex);
            if (owner.signals.size() == kSignalBudget) {
                owner.signals.erase(owner.signals.begin()); // Best effort; GNS retries.
            }
            owner.signals.push_back({remote, {bytes.begin(), bytes.end()}});
            return true;
        }
        void Release() override { released.store(true); }
    };
    static constexpr usize kSignalBudget = 64;
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket listener = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
    std::map<HSteamNetConnection, Link> links;
    std::vector<Event> events;
    Connection nextId = 1;
    bool initialized = false;
    bool ownsRuntime = false;
    bool trace = false;
    std::string identity;
    bool host = false;
    std::set<std::string> authorized;
    std::mutex signalMutex;
    std::vector<Signal> signals;
    std::vector<std::unique_ptr<Signaling>> adapters;

    Signaling* signaling(const std::string& remote) {
        std::erase_if(adapters, [](const auto& adapter) { return adapter->released.load(); });
        if (adapters.size() >= kMaxConnections * 2) {
            return nullptr;
        }
        adapters.push_back(std::make_unique<Signaling>(*this, remote));
        return adapters.back().get();
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        if (sockets) {
            for (const auto& [handle, link] : links) {
                sockets->CloseConnection(handle, 0, "Transport shutdown", false);
            }
            if (listener != k_HSteamListenSocket_Invalid) {
                sockets->CloseListenSocket(listener);
            }
            if (group != k_HSteamNetPollGroup_Invalid) {
                sockets->DestroyPollGroup(group);
            }
        }
        // RunCallbacks is the only callback dispatch point; Kill destroys pending
        // callbacks before the callback's user-data pointer can become dangling.
        if (initialized) {
            GameNetworkingSockets_Kill();
        }
        if (ownsRuntime) {
            runtimeOwned().store(false);
        }
    }

    auto find(Connection id) const {
        return std::ranges::find_if(links, [id](const auto& pair) { return pair.second.id == id; });
    }

    void disconnected(HSteamNetConnection handle, std::string reason) {
        const auto found = links.find(handle);
        if (found == links.end()) {
            return;
        }
        if (trace) {
            std::array<char, 8192> details{};
            if (sockets->GetDetailedConnectionStatus(handle, details.data(),
                                                     static_cast<s32>(details.size())) == 0) {
                log::info("Network connection {} closed: {}\n{}", found->second.id, reason,
                          details.data());
            }
        }
        events.push_back({EventType::Disconnected, found->second.id, {}, std::move(reason)});
        sockets->CloseConnection(handle, 0, nullptr, false);
        links.erase(found);
    }

    static void statusChangedCallback(SteamNetConnectionStatusChangedCallback_t* change) {
        // The listener's ConnectionUserData config is inherited by accepted links.
        // GNS delivers this callback on the thread calling poll(), never a worker.
        // This immutable local pointer is never populated from the network.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
        auto* impl = reinterpret_cast<Impl*>(change->m_info.m_nUserData);
        impl->statusChanged(*change);
    }

    void statusChanged(const SteamNetConnectionStatusChangedCallback_t& change) {
        const auto handle = change.m_hConn;
        if (trace) {
            log::info("Network connection {} state {} -> {}", handle,
                      static_cast<s32>(change.m_eOldState),
                      static_cast<s32>(change.m_info.m_eState));
        }
        switch (change.m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (change.m_info.m_hListenSocket != listener ||
                listener == k_HSteamListenSocket_Invalid) {
                return; // Outbound connection, already registered.
            }
            if (const auto existing = links.find(handle); existing != links.end()) {
                return; // P2P admission already accepted this authenticated identity.
            }
            if (!identity.empty() || links.size() >= kMaxConnections ||
                events.size() >= kReceiveBudget || !change.m_info.m_addrRemote.IsLocalHost()) {
                sockets->CloseConnection(handle, 0, "Listener capacity", false);
                return;
            }
            links.emplace(handle, Link{nextId++, false, {}});
            if (!sockets->SetConnectionPollGroup(handle, group) ||
                sockets->AcceptConnection(handle) != k_EResultOK) {
                disconnected(handle, "Accept failed");
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            if (auto found = links.find(handle); found != links.end() && !found->second.connected) {
                found->second.connected = true;
                events.push_back({EventType::Connected, found->second.id, {}, {}});
            }
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            disconnected(handle, change.m_info.m_szEndDebug);
            break;
        default: break;
        }
    }

    auto options(bool p2p = false) {
        std::array<SteamNetworkingConfigValue_t, 10> values{};
        // GNS's C configuration API represents callbacks as void* and userdata as int64.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                         reinterpret_cast<void*>(&Impl::statusChangedCallback));
        values[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                           reinterpret_cast<s64>(this));
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        values[2].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, kSendBufferBytes);
        values[3].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 5000);
        values[4].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, p2p ? 15000 : 5000);
        values[5].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize,
                           static_cast<s32>(kMaxPacketBytes));
        values[6].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, kSendBufferBytes);
        values[7].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages,
                           static_cast<s32>(kReceiveBudget));
        // Full native checkpoints exceed GNS's default 256 KiB/s (G1 alone
        // starts at ~15 KiB x 20/s). Keep the queue bounded at 64 KiB instead
        // of letting stale world states accumulate behind that default cap.
        // GNS 1.6 documents using equal min/max for its fixed-rate pacer.
        constexpr s32 kSendRate = 1024 * 1024;
        values[8].SetInt32(k_ESteamNetworkingConfig_SendRateMin, kSendRate);
        values[9].SetInt32(k_ESteamNetworkingConfig_SendRateMax, kSendRate);
        return values;
    }
};

std::unique_ptr<GnsTransport> GnsTransport::create(Simulation simulation, std::string& error) {
    error.clear();
    if (simulation.lagMs < 0 || simulation.lagMs > 5000 || !std::isfinite(simulation.lossPercent) ||
        simulation.lossPercent < 0 || simulation.lossPercent > 100) {
        error = "Invalid network simulation settings";
        return nullptr;
    }
    // GNS init/shutdown and simulation settings are process-global. Do not let a
    // second adapter silently reset another session's connections or test profile.
    auto impl = std::make_unique<Impl>();
    if (runtimeOwned().exchange(true)) {
        error = "A networking transport already owns this process";
        return nullptr;
    }
    impl->ownsRuntime = true;
    SteamNetworkingErrMsg message{};
    if (!GameNetworkingSockets_Init(nullptr, message)) {
        error = message;
        return nullptr;
    }
    impl->initialized = true;
    impl->trace = simulation.trace;
    // Do not enable vendor verbose logging even for trace runs: it dumps
    // rendezvous protobufs containing ICE passwords. Our trace logs only sizes,
    // state changes and connection statistics, never signaling contents.
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Warning,
        [](ESteamNetworkingSocketsDebugOutputType /*unused*/, const char* message) {
            log::warn("Network: {}", message);
        });
    impl->sockets = SteamNetworkingSockets();
    impl->group = impl->sockets->CreatePollGroup();
    if (impl->group == k_HSteamNetPollGroup_Invalid ||
        !SteamNetworkingUtils()->SetGlobalConfigValueInt32(
            k_ESteamNetworkingConfig_FakePacketLag_Send, simulation.lagMs) ||
        !SteamNetworkingUtils()->SetGlobalConfigValueFloat(
            k_ESteamNetworkingConfig_FakePacketLoss_Send, simulation.lossPercent)) {
        error = "Could not initialize networking queues or simulation settings";
        return nullptr;
    }
    return std::make_unique<GnsTransport>(ConstructionKey{}, std::move(impl));
}

GnsTransport::GnsTransport(ConstructionKey /*unused*/, std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
GnsTransport::~GnsTransport() = default;

std::optional<u16> GnsTransport::listenLoopback() {
    if (m_impl->listener != k_HSteamListenSocket_Invalid || !m_impl->identity.empty()) {
        return std::nullopt;
    }
    SteamNetworkingIPAddr address{};
    auto options = m_impl->options();
    // GNS 1.6 rejects port zero. Binding itself reserves the port atomically;
    // probing with another socket and closing it would introduce a port race.
    std::random_device random;
    std::uniform_int_distribution<u32> ports(49152, 65535);
    for (s32 attempt = 0; attempt < 32; ++attempt) {
        address.SetIPv4(0x7f000001U, static_cast<u16>(ports(random)));
        m_impl->listener = m_impl->sockets->CreateListenSocketIP(
            address, static_cast<s32>(options.size()), options.data());
        if (m_impl->listener != k_HSteamListenSocket_Invalid) {
            break;
        }
    }
    if (m_impl->listener == k_HSteamListenSocket_Invalid) {
        return std::nullopt;
    }
    return address.m_port;
}

std::optional<PacketTransport::Connection> GnsTransport::connectLoopback(u16 port) {
    if (!m_impl->identity.empty() || port == 0 || m_impl->links.size() >= kMaxConnections ||
        m_impl->events.size() >= kReceiveBudget) {
        return std::nullopt;
    }
    SteamNetworkingIPAddr address{};
    address.SetIPv4(0x7f000001U, port);
    auto options = m_impl->options();
    const auto handle = m_impl->sockets->ConnectByIPAddress(
        address, static_cast<s32>(options.size()), options.data());
    if (handle == k_HSteamNetConnection_Invalid) {
        return std::nullopt;
    }
    const auto id = m_impl->nextId++;
    m_impl->links.emplace(handle, Impl::Link{id, false, {}});
    if (!m_impl->sockets->SetConnectionPollGroup(handle, m_impl->group)) {
        m_impl->disconnected(handle, "Receive queue failed");
        return std::nullopt;
    }
    return id;
}

bool GnsTransport::configurePeer(std::string identity, bool host) {
    if (!validPeer(identity) || !m_impl->identity.empty() || !m_impl->links.empty() ||
        m_impl->listener != k_HSteamListenSocket_Invalid) {
        return false;
    }
    const auto local = networkIdentity(identity);
    if (local.IsInvalid()) {
        return false;
    }
    m_impl->sockets->ResetIdentity(&local);
    // ResetIdentity destroys poll groups as well as connections. Recreate the
    // receive group before assigning any P2P links to it.
    m_impl->group = m_impl->sockets->CreatePollGroup();
    if (m_impl->group == k_HSteamNetPollGroup_Invalid) {
        return false;
    }
    // Native ICE host candidates exercise rendezvous without credentials or
    // third-party traffic. Internet NAT traversal/relay is a separate test tier.
    if (!SteamNetworkingUtils()->SetGlobalConfigValueInt32(
            k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
            k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private) ||
        !SteamNetworkingUtils()->SetGlobalConfigValueString(
            k_ESteamNetworkingConfig_P2P_STUN_ServerList, "")) {
        return false;
    }
    if (host) {
        auto options = m_impl->options(true);
        m_impl->listener = m_impl->sockets->CreateListenSocketP2P(
            0, static_cast<s32>(options.size()), options.data());
        if (m_impl->listener == k_HSteamListenSocket_Invalid) {
            return false;
        }
    }
    m_impl->identity = std::move(identity);
    m_impl->host = host;
    return true;
}

bool GnsTransport::authorizePeers(std::span<const std::string> peers) {
    if (m_impl->identity.empty() || peers.size() > 3 || (!m_impl->host && peers.size() > 1) ||
        !std::ranges::all_of(
            peers, [&](const auto& peer) { return validPeer(peer) && peer != m_impl->identity; })) {
        return false;
    }
    const std::set<std::string> roster(peers.begin(), peers.end());
    if (roster.size() != peers.size()) {
        return false;
    }
    m_impl->authorized = roster;
    std::vector<Connection> removed;
    for (const auto& [handle, link] : m_impl->links) {
        if (!roster.contains(link.peer)) {
            removed.push_back(link.id);
        }
    }
    for (const auto id : removed) {
        close(id);
    }
    return true;
}

std::optional<PacketTransport::Connection> GnsTransport::connectPeer(const std::string& peer) {
    if (m_impl->host || !m_impl->authorized.contains(peer) ||
        m_impl->links.size() >= kMaxConnections || m_impl->events.size() >= kReceiveBudget ||
        std::ranges::any_of(m_impl->links,
                            [&](const auto& row) { return row.second.peer == peer; })) {
        return std::nullopt;
    }
    auto* signaling = m_impl->signaling(peer);
    if (!signaling) {
        return std::nullopt;
    }
    const auto remote = networkIdentity(peer);
    auto options = m_impl->options(true);
    const auto handle = m_impl->sockets->ConnectP2PCustomSignaling(
        signaling, &remote, 0, static_cast<s32>(options.size()), options.data());
    if (handle == k_HSteamNetConnection_Invalid) {
        return std::nullopt;
    }
    const auto id = m_impl->nextId++;
    m_impl->links.emplace(handle, Impl::Link{id, false, peer});
    if (!m_impl->sockets->SetConnectionPollGroup(handle, m_impl->group)) {
        m_impl->disconnected(handle, "Receive queue failed");
        return std::nullopt;
    }
    return id;
}

std::optional<std::string> GnsTransport::peer(Connection connection) const {
    const auto link = m_impl->find(connection);
    if (link == m_impl->links.end() || link->second.peer.empty()) {
        return std::nullopt;
    }
    return link->second.peer;
}

std::vector<GnsTransport::Signal> GnsTransport::takeSignals() {
    const std::scoped_lock lock(m_impl->signalMutex);
    return std::exchange(m_impl->signals, {});
}

bool GnsTransport::receiveSignal(const Signal& signal) {
    if (!m_impl->authorized.contains(signal.peer) || signal.bytes.empty() ||
        signal.bytes.size() > kMaxSignalBytes ||
        !signalIdentitiesMatch(signal.bytes, signal.peer, m_impl->identity)) {
        return false;
    }
    struct Context final : ISteamNetworkingSignalingRecvContext {
        Impl& impl;
        const std::string& sender;
        Context(Impl& owner, const std::string& peer) : impl(owner), sender(peer) {}
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection handle,
                                                              const SteamNetworkingIdentity& remote,
                                                              int port) override {
            if (!impl.host || port != 0 || !(remote == networkIdentity(sender)) ||
                impl.links.size() >= kMaxConnections || impl.events.size() >= kReceiveBudget ||
                std::ranges::any_of(impl.links,
                                    [&](const auto& row) { return row.second.peer == sender; })) {
                return nullptr;
            }
            auto* adapter = impl.signaling(sender);
            if (!adapter) {
                return nullptr;
            }
            impl.links.emplace(handle, Impl::Link{impl.nextId++, false, sender});
            if (!impl.sockets->SetConnectionPollGroup(handle, impl.group) ||
                impl.sockets->AcceptConnection(handle) != k_EResultOK) {
                impl.disconnected(handle, "P2P accept failed");
                adapter->Release();
                return nullptr;
            }
            return adapter;
        }
        void SendRejectionSignal(const SteamNetworkingIdentity& /*unused*/, const void* /*unused*/,
                                 int /*unused*/) override {}
    } context(*m_impl, signal.peer);
    return m_impl->sockets->ReceivedP2PCustomSignal(
        signal.bytes.data(), static_cast<s32>(signal.bytes.size()), &context);
}

PacketTransport::SendResult GnsTransport::send(Connection connection, std::span<const u8> bytes,
                                               Delivery delivery) {
    if (bytes.empty() || bytes.size() > kMaxPacketBytes) {
        return SendResult::Invalid;
    }
    const auto found = m_impl->find(connection);
    if (found == m_impl->links.end() || !found->second.connected) {
        return SendResult::Disconnected;
    }
    const auto result = m_impl->sockets->SendMessageToConnection(
        found->first, bytes.data(), static_cast<u32>(bytes.size()),
        delivery == Delivery::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
                                       : k_nSteamNetworkingSend_UnreliableNoDelay,
        nullptr);
    if (result == k_EResultOK) {
        return SendResult::Sent;
    }
    if (result == k_EResultLimitExceeded || result == k_EResultIgnored) {
        return SendResult::Congested;
    }
    return SendResult::Disconnected;
}

std::vector<PacketTransport::Event> GnsTransport::poll() {
    m_impl->sockets->RunCallbacks();
    auto events = std::exchange(m_impl->events, {});
    std::array<SteamNetworkingMessage_t*, 32> batch{};
    for (usize received = 0; received < kReceiveBudget; received += batch.size()) {
        const s32 count = m_impl->sockets->ReceiveMessagesOnPollGroup(
            m_impl->group, batch.data(), static_cast<s32>(batch.size()));
        if (count <= 0) {
            break;
        }
        for (s32 i = 0; i < count; ++i) {
            auto* message = batch[static_cast<usize>(i)];
            const auto found = m_impl->links.find(message->m_conn);
            if (found != m_impl->links.end()) {
                if (message->m_cbSize <= 0 ||
                    static_cast<usize>(message->m_cbSize) > kMaxPacketBytes) {
                    m_impl->disconnected(message->m_conn, "Oversized or empty packet");
                } else {
                    const std::span data(static_cast<const u8*>(message->m_pData),
                                         static_cast<usize>(message->m_cbSize));
                    events.push_back(
                        {EventType::Message, found->second.id, {data.begin(), data.end()}, {}});
                }
            }
            message->Release();
        }
    }
    return events;
}

std::optional<PacketTransport::Statistics> GnsTransport::statistics(Connection connection) const {
    const auto found = m_impl->find(connection);
    if (found == m_impl->links.end()) {
        return std::nullopt;
    }
    SteamNetConnectionRealTimeStatus_t status{};
    if (m_impl->sockets->GetConnectionRealTimeStatus(found->first, &status, 0, nullptr) !=
        k_EResultOK) {
        return std::nullopt;
    }
    return Statistics{status.m_nPing, status.m_cbPendingReliable + status.m_cbPendingUnreliable,
                      status.m_usecQueueTime};
}

void GnsTransport::close(Connection connection) {
    const auto found = m_impl->find(connection);
    if (found != m_impl->links.end()) {
        m_impl->disconnected(found->first, "Closed locally");
    }
}

} // namespace gdl
