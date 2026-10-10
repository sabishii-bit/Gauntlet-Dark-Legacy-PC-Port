#include <algorithm>
#include <limits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/OnlineSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OnlineSession::Phase;
using Failure = OnlineSession::Failure;
using Connection = PacketTransport::Connection;
using Event = PacketTransport::Event;
using EventType = PacketTransport::EventType;
const std::string kHost(32, 'a');
const std::string kGuest(32, 'b');
const std::string kOther(32, 'c');

struct Service final : RoomService {
    Update next;
    std::vector<std::pair<u64, bool>> readiness;
    std::vector<u64> starts;
    std::vector<PeerTransport::Signal> signals;
    bool accepts = true;
    usize leaves = 0;
    Update poll() override { return std::exchange(next, {}); }
    bool send(PeerTransport::Signal signal) override {
        signals.push_back(std::move(signal));
        return accepts;
    }
    bool ready(u64 revision, bool value) override {
        if (accepts) {
            readiness.emplace_back(revision, value);
        }
        return accepts;
    }
    bool start(u64 revision) override {
        if (accepts) {
            starts.push_back(revision);
        }
        return accepts;
    }
    void leave() override { ++leaves; }
};
struct Wire final : PeerTransport {
    struct Sent {
        Connection connection;
        std::vector<u8> bytes;
    };
    std::string identity;
    bool host = false;
    bool working = true;
    SendResult result = SendResult::Sent;
    std::vector<std::string> authorized;
    std::vector<Event> incoming;
    std::vector<Sent> sent;
    std::vector<Connection> closed;
    std::map<Connection, std::string> peers;
    std::vector<Signal> signals;
    usize signalCount = 0;
    usize configureCount = 0;
    bool configurePeer(std::string peer, bool isHost) override {
        identity = std::move(peer);
        host = isHost;
        ++configureCount;
        return working;
    }
    bool authorizePeers(std::span<const std::string> allowed) override {
        authorized.assign(allowed.begin(), allowed.end());
        for (const auto& [connection, peer] : peers) {
            if (std::ranges::find(authorized, peer) == authorized.end()) {
                close(connection);
            }
        }
        return working;
    }
    std::optional<Connection> connectPeer(const std::string& peer) override {
        peers[99] = peer;
        return working ? std::optional<Connection>{99} : std::nullopt;
    }
    std::optional<std::string> peer(Connection connection) const override {
        const auto found = peers.find(connection);
        return found == peers.end() ? std::nullopt : std::optional{found->second};
    }
    std::vector<Signal> takeSignals() override { return std::exchange(signals, {}); }
    bool receiveSignal(const Signal& /*signal*/) override {
        ++signalCount;
        return working;
    }
    SendResult send(Connection connection, std::span<const u8> bytes,
                    Delivery /*delivery*/) override {
        if (result == SendResult::Sent) {
            sent.push_back({connection, {bytes.begin(), bytes.end()}});
        }
        return result;
    }
    std::vector<Event> poll() override { return std::exchange(incoming, {}); }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection connection) override { closed.push_back(connection); }
    void connect(const std::string& peer, Connection connection) {
        peers[connection] = peer;
        incoming.push_back({EventType::Connected, connection, {}, {}});
    }
    void message(Connection connection, const std::vector<u8>& bytes) {
        incoming.push_back({EventType::Message, connection, bytes, {}});
    }
};
RoomSnapshot room() {
    return {"ABCDEFGH", kHost, 1, false, {{kHost, {0}, false}, {kGuest, {1}, false}}, {}};
}
std::vector<u8> partyPacket(u8 kind, u8 seat = 0) {
    std::vector<u8> bytes{'G', 'D', 'P', 'S', 1, kind, seat, 0};
    if (kind == 0) {
        CharacterProfile profile;
        profile.name = "TEST";
        const auto encoded = CharacterProfilePacket::encode(profile);
        REQUIRE(encoded);
        bytes.insert(bytes.end(), encoded->begin(), encoded->end());
    }
    return bytes;
}
struct Client {
    Service service;
    Wire wire;
    OnlineSession session;
    explicit Client(bool host = true, u8 localPlayers = 1)
        : session(service, wire, host, localPlayers) {
        if (localPlayers > 0 && localPlayers <= 4) {
            std::vector<CharacterProfile> profiles(localPlayers);
            for (auto& profile : profiles) {
                profile.name = "TEST";
            }
            REQUIRE(session.select(profiles));
        }
    }
    void finishParty(const RoomSnapshot& snapshot) {
        if (!snapshot.started || session.phase() != Phase::Starting || !session.connected()) {
            return;
        }
        for (const auto& member : snapshot.members) {
            if (session.host() && member.peer == kHost) {
                continue;
            }
            const auto link = std::ranges::find_if(wire.peers, [&](const auto& row) {
                return row.second == (session.host() ? member.peer : kHost);
            });
            REQUIRE(link != wire.peers.end());
            for (const u8 seat : member.seats) {
                wire.message(link->first, partyPacket(0, seat));
            }
        }
        session.update(0);
        if (session.host()) {
            for (const auto& [connection, peer] : wire.peers) {
                if (std::ranges::find(snapshot.members, peer, &RoomMember::peer) !=
                    snapshot.members.end()) {
                    wire.message(connection, partyPacket(2));
                }
            }
        } else {
            wire.message(99, partyPacket(1));
        }
        session.update(0);
        std::erase_if(wire.sent,
                      [](const auto& sent) { return PartyBootstrap::recognizes(sent.bytes); });
    }
    void publish(RoomSnapshot snapshot) {
        service.next = {snapshot, session.host() ? kHost : kGuest, {}, false};
        session.update(0);
        finishParty(snapshot);
    }
    void connect() {
        wire.connect(session.host() ? kGuest : kHost, session.host() ? 1 : 99);
        session.update(0);
    }
};
std::vector<u8> prepare(MatchOwners owners = {1, 2, 0, 0}) {
    MatchContext context;
    context.epoch = 2;
    context.scene = 7;
    context.owners = owners;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        context.grants[seat] = owners[seat] == 0 ? 0 : 1;
    }
    const auto bytes =
        MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
    REQUIRE(bytes);
    return *bytes;
}

TEST_CASE("online room readiness waits for authenticated links and server confirmation",
          "[netplay][online-session]") {
    Client host;
    CHECK_FALSE(host.session.ready());
    CHECK_FALSE(host.session.start());
    host.publish(room());
    CHECK(host.session.phase() == Phase::Lobby);
    CHECK(host.wire.authorized == std::vector{kGuest});
    CHECK_FALSE(host.session.ready());
    CHECK_FALSE(host.session.start());
    host.connect();
    REQUIRE(host.session.connected());
    REQUIRE(host.session.ready());
    REQUIRE(host.session.ready());
    REQUIRE(host.service.readiness.size() == 1);
    CHECK(host.service.readiness.front() == std::pair<u64, bool>{1, true});
    CHECK_FALSE(host.session.start());
    auto ready = room();
    for (auto& member : ready.members) {
        member.ready = true;
    }
    host.publish(ready);
    REQUIRE(host.session.start());
    CHECK_FALSE(host.session.start());
    CHECK(host.service.starts == std::vector<u64>{1});
    CHECK(host.session.phase() == Phase::Starting);
    CHECK(host.session.match().phase() == MatchSession::Phase::Offline);
    CHECK_FALSE(host.session.match().prepare(7));
    ready.started = true;
    host.publish(ready);
    CHECK(host.session.phase() == Phase::Active);
    REQUIRE(host.session.match().prepare(7));
    CHECK(host.session.match().context().owners == MatchOwners{1, 2, 0, 0});
    CHECK(host.wire.configureCount == 1);
}

TEST_CASE("online sessions handle split local seats, sparse rosters and unordered replies",
          "[netplay][online-session]") {
    Client host(true, 2);
    Client guest(false, 1);
    auto snapshot = room();
    snapshot.members = {{kGuest, {3}, true}, {kHost, {2, 0}, true}};
    host.publish(snapshot);
    guest.publish(snapshot);
    host.connect();
    guest.connect();
    CHECK_FALSE(guest.session.start());
    REQUIRE(host.session.start());
    snapshot.started = true;
    host.publish(snapshot);
    std::ranges::reverse(snapshot.members);
    guest.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    host.session.update(0);
    REQUIRE(host.wire.sent.size() == 1);
    guest.wire.message(99, host.wire.sent.front().bytes);
    guest.session.update(0);
    CHECK(guest.session.match().context().owners == MatchOwners{1, 0, 1, 2});
    CHECK(std::ranges::equal(host.session.seats(), std::array<u8, 2>{0, 2}));
    CHECK(std::ranges::equal(guest.session.seats(), std::array<u8, 1>{3}));
    REQUIRE(host.session.match().loaded());
    CHECK_FALSE(host.session.match().advance());
    REQUIRE(guest.session.match().loaded());
    guest.session.update(0);
    REQUIRE(guest.wire.sent.size() == 1);
    host.wire.sent.clear();
    host.wire.message(1, guest.wire.sent.front().bytes);
    host.session.update(0);
    REQUIRE(host.wire.sent.size() == 1);
    guest.wire.message(99, host.wire.sent.front().bytes);
    guest.session.update(0);
    CHECK(host.session.match().phase() == MatchSession::Phase::Running);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Running);
    CHECK_FALSE(guest.session.match().advance());
    CHECK(host.session.match().advance());
}

TEST_CASE("online host has peer one even when it does not occupy seat zero",
          "[netplay][online-session]") {
    Client host;
    auto snapshot = room();
    snapshot.members = {{kGuest, {0}, true}, {kHost, {3}, true}, {kOther, {1}, true}};
    host.publish(snapshot);
    host.connect();
    host.wire.connect(kOther, 2);
    host.session.update(0);
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    CHECK(host.session.match().context().owners == MatchOwners{2, 3, 0, 1});
}

TEST_CASE("an early host prepare waits for the started roster and is not lost on a newer revision",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    auto snapshot = room();
    MatchOwners owners{1, 2, 0, 0};
    SECTION("same roster") {}
    SECTION("room service coalesced join and start") {
        ++snapshot.revision;
        snapshot.members.push_back({kOther, {2}, true});
        owners[2] = 3;
    }
    guest.wire.message(99, prepare(owners));
    guest.session.update(0);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
    CHECK_FALSE(guest.session.match().loaded());
    snapshot.started = true;
    guest.publish(snapshot);
    REQUIRE(guest.session.phase() == Phase::Active);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Loading);
    CHECK(guest.session.match().context().owners == owners);
}

TEST_CASE("early match data cannot bypass room start or the admitted owners",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    SECTION("arbitrary data and foreign connection are discarded") {
        for (usize i = 0; i < 200; ++i) {
            guest.wire.message(99, std::vector<u8>(1200, 0xff));
            guest.wire.message(42, prepare());
            guest.session.update(0);
        }
        CHECK(guest.session.phase() == Phase::Lobby);
        CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
    }
    SECTION("a conflicting second prepare fails closed") {
        guest.wire.message(99, prepare());
        guest.wire.message(99, prepare({1, 3, 0, 0}));
        guest.session.update(0);
        CHECK(guest.session.failure() == Failure::Protocol);
    }
    SECTION("a forged owner roster cannot be promoted") {
        guest.wire.message(99, prepare({1, 3, 0, 0}));
        guest.session.update(0);
        auto snapshot = room();
        snapshot.started = true;
        guest.publish(snapshot);
        CHECK(guest.session.failure() == Failure::Protocol);
        CHECK_FALSE(guest.session.match().advance());
    }
}

TEST_CASE("roster changes invalidate a pending start and require readiness again",
          "[netplay][online-session]") {
    Client host;
    auto snapshot = room();
    for (auto& member : snapshot.members) {
        member.ready = true;
    }
    host.publish(snapshot);
    host.connect();
    REQUIRE(host.session.start());
    ++snapshot.revision;
    snapshot.members.push_back({kOther, {2}, false});
    for (auto& member : snapshot.members) {
        member.ready = false;
    }
    host.publish(snapshot);
    CHECK(host.session.phase() == Phase::Lobby);
    CHECK_FALSE(host.session.ready());
    CHECK_FALSE(host.session.start());
    host.wire.connect(kOther, 2);
    host.session.update(0);
    REQUIRE(host.session.ready());
    CHECK(host.service.readiness.back().first == 2);
    CHECK(host.session.match().phase() == MatchSession::Phase::Offline);
}

TEST_CASE("lobby departure closes old links without inheriting their seat authority",
          "[netplay][online-session]") {
    Client host;
    auto snapshot = room();
    host.publish(snapshot);
    host.connect();
    SECTION("transport disconnect arrives first") {
        host.wire.incoming.push_back({EventType::Disconnected, 1, {}, {}});
        host.session.update(0);
        CHECK(host.session.phase() == Phase::Lobby);
        CHECK_FALSE(host.session.connected());
        CHECK_FALSE(host.session.start());
    }
    SECTION("room update arrives first") {}
    snapshot.members[1].peer = kOther;
    ++snapshot.revision;
    host.publish(snapshot);
    CHECK(std::ranges::find(host.wire.closed, 1) != host.wire.closed.end());
    host.wire.message(1, prepare());
    host.wire.incoming.push_back({EventType::Disconnected, 1, {}, {}});
    host.session.update(0);
    CHECK_FALSE(host.session.connected());
    CHECK(host.session.phase() == Phase::Lobby);
    host.wire.connect(kOther, 2);
    host.session.update(0);
    REQUIRE(host.session.connected());
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    host.session.update(0);
    REQUIRE(host.wire.sent.size() == 1);
    CHECK(host.wire.sent.front().connection == 2);
}

TEST_CASE("a guest unready racing host start returns to the lobby without a timeout",
          "[netplay][online-session]") {
    Client host;
    auto snapshot = room();
    for (auto& member : snapshot.members) {
        member.ready = true;
    }
    host.publish(snapshot);
    host.connect();
    REQUIRE(host.session.start());
    snapshot.members[1].ready = false;
    host.publish(snapshot);
    CHECK(host.session.phase() == Phase::Lobby);
    CHECK_FALSE(host.session.start());
    snapshot.members[1].ready = true;
    host.publish(snapshot);
    REQUIRE(host.session.start());
    CHECK(host.service.starts.size() == 2);
}

TEST_CASE("authoritative stop wins over stale gameplay in the same online receive batch",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    guest.wire.message(99, prepare());
    guest.session.update(0);
    auto snapshot = room();
    snapshot.started = true;
    guest.publish(snapshot);
    REQUIRE(guest.session.match().phase() == MatchSession::Phase::Loading);
    const auto stop = MatchControlPacket::encode(
        {MatchControlKind::Stop, MatchStop::Left, guest.session.match().context()});
    REQUIRE(stop);
    guest.wire.message(99, *stop);
    guest.wire.message(99, std::vector<u8>{1, 2, 3});
    guest.session.update(0);
    CHECK(guest.session.failure() == Failure::Match);
    CHECK(guest.session.match().stopReason() == MatchStop::Left);
}

TEST_CASE("online admission rejects malformed or changed identities atomically",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    auto invalid = room();
    SECTION("duplicate seat") {
        invalid.members[1].seats = {0};
    }
    SECTION("invalid seat") {
        invalid.members[1].seats = {4};
    }
    SECTION("duplicate identity") {
        invalid.members[1].peer = kHost;
    }
    SECTION("changed local seats") {
        invalid.members[0].seats = {2};
    }
    SECTION("changed local player count") {
        invalid.members[0].seats = {0, 2};
    }
    SECTION("changed host") {
        invalid.host = kGuest;
    }
    SECTION("changed room") {
        invalid.code = "BCDEFGHJ";
    }
    SECTION("rollback") {
        invalid.revision = 0;
    }
    SECTION("invalid peer") {
        invalid.members[1].peer = "bad";
    }
    SECTION("member changed without revision") {
        invalid.members[1].peer = kOther;
    }
    SECTION("no members") {
        invalid.members.clear();
    }
    host.publish(invalid);
    CHECK(host.session.phase() == Phase::Failed);
    REQUIRE(host.session.room());
    CHECK(host.session.room()->members == room().members);
    CHECK(host.wire.configureCount == 1);
    CHECK(host.service.leaves == 1);
}

TEST_CASE("online state fails closed after a started member departs or changes",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    auto snapshot = room();
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    SECTION("disconnect") {
        host.wire.incoming.push_back({EventType::Disconnected, 1, {}, {}});
        host.session.update(0);
        CHECK(host.session.failure() == Failure::Transport);
    }
    SECTION("service removal") {
        snapshot.members.pop_back();
        ++snapshot.revision;
        host.publish(snapshot);
        CHECK(host.session.failure() == Failure::RosterChanged);
    }
    SECTION("service failure") {
        host.service.next.error = "private response not for UI";
        host.session.update(0);
        CHECK(host.session.failure() == Failure::Service);
    }
    SECTION("unexpected service close") {
        host.service.next.closed = true;
        host.session.update(0);
        CHECK(host.session.failure() == Failure::Service);
    }
    CHECK(host.session.phase() == Phase::Failed);
    CHECK(host.session.match().phase() == MatchSession::Phase::Stopped);
    CHECK_FALSE(host.session.match().advance());
    CHECK_FALSE(host.session.ready());
    host.session.leave();
    CHECK(host.service.leaves == 1);
}

TEST_CASE("online session timeouts never require blocking the frame thread",
          "[netplay][online-session]") {
    Client host;
    SECTION("admission") {
        host.session.update(OnlineSession::kAdmissionTimeout);
    }
    SECTION("connection") {
        host.publish(room());
        host.session.update(OnlineSession::kConnectionTimeout);
    }
    SECTION("start confirmation") {
        auto snapshot = room();
        for (auto& member : snapshot.members) {
            member.ready = true;
        }
        host.publish(snapshot);
        host.connect();
        REQUIRE(host.session.start());
        host.session.update(OnlineSession::kStartTimeout);
    }
    CHECK(host.session.phase() == Phase::Failed);
    CHECK(host.session.failure() == Failure::Timeout);
    CHECK(host.service.leaves == 1);
}

TEST_CASE("online leave is asynchronous idempotent and bounded even without a service reply",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    host.session.leave();
    host.session.leave();
    CHECK(host.session.phase() == Phase::Leaving);
    CHECK(host.service.leaves == 1);
    CHECK(host.wire.closed == std::vector<Connection>{1});
    SECTION("worker closes") {
        host.service.next.closed = true;
    }
    SECTION("worker stalls") {
        host.session.update(OnlineSession::kLeaveTimeout);
    }
    host.session.update(0);
    CHECK(host.session.phase() == Phase::Closed);
    CHECK(host.session.failure() == Failure::None);
}

TEST_CASE("online room UI never retains signaling credentials or routes unadmitted signals",
          "[netplay][online-session]") {
    Client host;
    auto snapshot = room();
    snapshot.signals = {{kGuest, {1}}, {kOther, {2}}};
    host.wire.signals = snapshot.signals;
    host.publish(snapshot);
    REQUIRE(host.session.room());
    CHECK(host.session.room()->signals.empty());
    CHECK(host.wire.signalCount == 1);
    REQUIRE(host.service.signals.size() == 1);
    CHECK(host.service.signals.front().peer == kGuest);
}

TEST_CASE("unknown and duplicate online connections never gain control of an admitted seat",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    host.wire.connect(kOther, 2);
    host.wire.connect(kGuest, 3);
    host.session.update(0);
    CHECK(host.wire.closed == std::vector<Connection>{2, 3});
    REQUIRE(host.session.connected());
    auto snapshot = room();
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    host.session.update(0);
    REQUIRE(host.wire.sent.size() == 1);
    CHECK(host.wire.sent.front().connection == 1);
}

TEST_CASE("bounded ready/start mailboxes can be retried without premature local state changes",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    host.service.accepts = false;
    CHECK_FALSE(host.session.ready());
    CHECK(host.session.phase() == Phase::Lobby);
    host.service.accepts = true;
    CHECK(host.session.ready());
    auto snapshot = room();
    for (auto& member : snapshot.members) {
        member.ready = true;
    }
    host.publish(snapshot);
    host.service.accepts = false;
    CHECK_FALSE(host.session.start());
    CHECK(host.session.phase() == Phase::Lobby);
    host.service.accepts = true;
    REQUIRE(host.session.ready(false));
    CHECK_FALSE(host.session.start());
    snapshot.members.front().ready = false;
    host.publish(snapshot);
    CHECK_FALSE(host.session.start());
}

TEST_CASE("online session rejects invalid local counts and time deltas",
          "[netplay][online-session]") {
    SECTION("no local players") {
        const Client client(true, 0);
        CHECK(client.session.failure() == Failure::Admission);
    }
    SECTION("too many local players") {
        const Client client(true, 5);
        CHECK(client.session.failure() == Failure::Admission);
    }
    SECTION("invalid delta") {
        for (const f64 delta :
             {-1.0, std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()}) {
            Client client;
            client.session.update(delta);
            CHECK(client.session.failure() == Failure::Protocol);
        }
    }
}
TEST_CASE("online readiness requires valid selections and freezes them while ready",
          "[netplay][online-session]") {
    Service service;
    Wire wire;
    OnlineSession session(service, wire, true, 1);
    service.next = {room(), kHost, {}, false};
    wire.connect(kGuest, 1);
    session.update(0);
    REQUIRE(session.connected());
    CHECK_FALSE(session.ready());
    CHECK_FALSE(session.select({}));
    CharacterProfile selected;
    selected.name = "LOCAL";
    REQUIRE(session.select(std::array{selected}));
    REQUIRE(session.ready());
    selected.name = "EDITED";
    CHECK_FALSE(session.select(std::array{selected}));
    auto snapshot = room();
    snapshot.members.front().ready = true;
    service.next = {snapshot, kHost, {}, false};
    session.update(0);
    CHECK_FALSE(session.select(std::array{selected}));
    REQUIRE(session.ready(false));
    CHECK_FALSE(session.select(std::array{selected}));
    snapshot.members.front().ready = false;
    service.next = {snapshot, kHost, {}, false};
    session.update(0);
    REQUIRE(session.select(std::array{selected}));
    snapshot.started = true;
    service.next = {snapshot, kHost, {}, false};
    session.update(0);
    CHECK_FALSE(session.select(std::array{selected}));
    CHECK(session.party() == nullptr);
    CHECK_FALSE(session.match().prepare(1));
}
TEST_CASE("early character relay and prepare survive a delayed room start reply",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    guest.wire.message(99, partyPacket(0, 0));
    guest.wire.message(99, partyPacket(0, 1));
    guest.wire.message(99, partyPacket(1));
    guest.wire.message(99, prepare());
    guest.session.update(0);
    CHECK(guest.session.party() == nullptr);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
    auto snapshot = room();
    snapshot.started = true;
    guest.publish(snapshot);
    REQUIRE(guest.session.phase() == Phase::Active);
    REQUIRE(guest.session.party());
    CHECK((*guest.session.party())[1]->name == "TEST");
    CHECK(guest.session.match().phase() == MatchSession::Phase::Loading);
}
TEST_CASE("missing party acknowledgements time out without exposing simulation authority",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    auto snapshot = room();
    snapshot.started = true;
    host.service.next = {snapshot, kHost, {}, false};
    host.session.update(0);
    host.wire.message(1, partyPacket(0, 1));
    host.session.update(0);
    CHECK(host.session.phase() == Phase::Starting);
    CHECK(host.session.party() == nullptr);
    CHECK_FALSE(host.session.match().prepare(1));
    CHECK_FALSE(host.session.match().advance());
    host.session.update(OnlineSession::kStartTimeout);
    CHECK(host.session.failure() == Failure::Timeout);
}
TEST_CASE("early character messages are bounded and cannot claim unowned seats",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    SECTION("bounded early relay") {
        for (usize i = 0; i < 6; ++i) {
            guest.wire.message(99, partyPacket(0, 0));
        }
        guest.session.update(0);
    }
    SECTION("unknown seat in otherwise valid early relay") {
        guest.wire.message(99, partyPacket(0, 3));
        guest.session.update(0);
        auto snapshot = room();
        snapshot.started = true;
        guest.publish(snapshot);
    }
    CHECK(guest.session.failure() == Failure::Protocol);
    CHECK(guest.session.party() == nullptr);
}
} // namespace
