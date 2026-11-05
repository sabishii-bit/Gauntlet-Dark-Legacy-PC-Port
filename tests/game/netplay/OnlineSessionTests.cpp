#include <algorithm>
#include <limits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/OnlinePair.h"
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
OnlineSession::NativeIdentity nativeIdentity() {
    return {"test-executable", "test-runtime", std::string(64, 'a'), std::string(64, 'b'), "en"};
}
bool nativeBootstrap(std::span<const u8> bytes) {
    return bytes.size() >= 8 && bytes[0] == 'G' && bytes[1] == 'D' && bytes[2] == 'N' &&
           bytes[3] == 'B';
}

struct Service final : RoomService {
    Update next;
    std::vector<std::pair<u64, bool>> readiness;
    std::vector<u64> starts;
    bool accepts = true;
    usize leaves = 0;
    Update poll() override { return std::exchange(next, {}); }
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
    Connection nextDialled = 99;
    std::optional<Connection> connectPeer(const std::string& peer) override {
        for (const auto& [connection, known] : peers) {
            if (known == peer) {
                return working ? std::optional<Connection>{connection} : std::nullopt;
            }
        }
        const auto connection = nextDialled--;
        peers[connection] = peer;
        return working ? std::optional<Connection>{connection} : std::nullopt;
    }
    std::optional<std::string> peer(Connection connection) const override {
        const auto found = peers.find(connection);
        return found == peers.end() ? std::nullopt : std::optional{found->second};
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
    return {"ABCDEFGH", kHost, 1, false, {{kHost, {0}, false}, {kGuest, {1}, false}}};
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
    test::RollbackSessionHarness timeline;
    explicit Client(bool host = true, u8 localPlayers = 1)
        : session(service, wire, host, localPlayers) {
        if (localPlayers > 0 && localPlayers <= 4) {
            OnlineSession::NativeRules rules;
            rules.scene = 7;
            REQUIRE(session.configureNative(nativeIdentity(),
                                            host ? std::optional{rules} : std::nullopt));
            std::vector<CharacterProfile> profiles(localPlayers);
            for (auto& profile : profiles) {
                profile.name = "TEST";
            }
            REQUIRE(session.select(profiles));
        }
    }
    void finishParty(const RoomSnapshot& snapshot, bool retainNative = false) {
        if (!snapshot.started || session.phase() != Phase::Starting || !session.connected()) {
            return;
        }
        // The early-relay fixture may have already delivered the complete party
        // and its one-shot seal before the local room-start notification.
        if (session.party() == nullptr) {
            for (const auto& member : snapshot.members) {
                if (session.host() && member.endpointId == kHost) {
                    continue;
                }
                const auto link = std::ranges::find_if(wire.peers, [&](const auto& row) {
                    return row.second == (session.host() ? member.endpointId : kHost);
                });
                REQUIRE(link != wire.peers.end());
                for (const u8 seat : member.seats) {
                    wire.message(link->first, partyPacket(0, seat));
                }
            }
            session.update(0);
            if (session.phase() == Phase::Failed) {
                return;
            }
            if (session.host()) {
                for (const auto& [connection, peer] : wire.peers) {
                    if (std::ranges::find(snapshot.members, peer, &RoomMember::endpointId) !=
                        snapshot.members.end()) {
                        wire.message(connection, partyPacket(2));
                    }
                }
            } else {
                wire.message(99, partyPacket(1));
            }
            session.update(0);
            if (session.phase() == Phase::Failed) {
                return;
            }
        }
        // These lifecycle tests use a fixed valid setup digest; the separate
        // native-bootstrap suite compares the complete resolved gameplay setup.
        const std::string setup(64, 'c');
        if (session.host()) {
            REQUIRE(session.nativeSetupPending());
            REQUIRE(session.confirmNativeSetup(setup));
            session.update(0);
            const auto seal = std::ranges::find_if(wire.sent, [](const auto& sent) {
                return nativeBootstrap(sent.bytes) && sent.bytes[5] == 1;
            });
            REQUIRE(seal != wire.sent.end());
            auto acknowledgment = seal->bytes;
            acknowledgment[5] = 2;
            for (const auto& [connection, peer] : wire.peers) {
                if (std::ranges::find(snapshot.members, peer, &RoomMember::endpointId) !=
                    snapshot.members.end()) {
                    wire.message(connection, acknowledgment);
                }
            }
            session.update(0);
        } else {
            // Produce the authenticated offer/seal/commit through the real host
            // bootstrap, not a parallel room-scope hash implementation.
            const auto host = std::ranges::find(snapshot.members, kHost, &RoomMember::endpointId);
            REQUIRE(host != snapshot.members.end());
            Client authority(true, static_cast<u8>(host->seats.size()));
            auto lobby = snapshot;
            lobby.started = false;
            authority.publish(lobby);
            Connection connection = 1;
            for (const auto& member : snapshot.members) {
                if (member.endpointId != kHost) {
                    authority.wire.connect(member.endpointId, connection++);
                }
            }
            authority.session.update(0);
            authority.service.next = {snapshot, kHost, {}, false};
            authority.session.update(0);
            authority.finishParty(snapshot, true);
            for (u8 kind : std::array<u8, 3>{0, 1, 3}) {
                const auto packet =
                    std::ranges::find_if(authority.wire.sent, [kind](const auto& sent) {
                        return nativeBootstrap(sent.bytes) && sent.bytes[5] == kind;
                    });
                REQUIRE(packet != authority.wire.sent.end());
                wire.message(99, packet->bytes);
                session.update(0);
                if (kind == 0) {
                    REQUIRE(session.nativeSetupPending());
                    REQUIRE(session.confirmNativeSetup(setup));
                }
            }
        }
        REQUIRE(session.phase() == Phase::Active);
        std::erase_if(wire.sent, [retainNative](const auto& sent) {
            return PartyBootstrap::recognizes(sent.bytes) ||
                   (!retainNative && nativeBootstrap(sent.bytes));
        });
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
std::vector<InputCommand> inputs(const MatchSession& session,
                                 const test::RollbackSessionHarness& timeline,
                                 bool pressed = false) {
    std::vector<InputCommand> result;
    const auto& context = session.context();
    for (usize seat = 0; seat < context.owners.size(); ++seat) {
        if (context.owners[seat] != session.localPeer()) {
            continue;
        }
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = timeline.sampleTick();
        command.grant = context.grants[seat];
        command.seat = static_cast<u8>(seat);
        command.direction = {1, 0};
        command.magnitude = 0.5f;
        command.pressedButtons = pressed ? static_cast<u32>(CommandPress::UsePotion) : 0;
        result.push_back(command);
    }
    return result;
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

TEST_CASE("room admission configures identity once without reconfiguring its provider",
          "[netplay][online-session]") {
    Client host;
    host.service.next = {room(), kHost, {}, false};
    host.session.update(0);
    CHECK(host.wire.identity == kHost);
    CHECK(host.wire.configureCount == 1);
    host.publish(room());
    CHECK(host.wire.identity == kHost);
    CHECK(host.wire.configureCount == 1);
}

TEST_CASE("post-frame flushing sends once without polling or advancing clocks",
          "[netplay][online-session][latency]") {
    Client host;
    host.publish(room());
    host.connect();
    auto started = room();
    started.started = true;
    for (auto& member : started.members) {
        member.ready = true;
    }
    host.publish(started);
    REQUIRE(host.session.phase() == Phase::Active);
    host.wire.sent.clear();
    REQUIRE(host.session.match().prepare(7));
    host.service.next.error = "must not poll during flush";
    const auto tick = host.session.match().nativeHead();
    const auto epoch = host.session.match().context().epoch;

    SECTION("congested output survives until the next flush") {
        host.wire.result = PacketTransport::SendResult::Congested;
        REQUIRE(host.session.flush());
        CHECK(host.wire.sent.empty());
        CHECK(host.session.match().queuedControls() == 1);
        host.wire.result = PacketTransport::SendResult::Sent;
        REQUIRE(host.session.flush());
        CHECK(host.wire.sent.size() == 1);
        CHECK(host.session.match().queuedControls() == 0);
        REQUIRE(host.session.flush());
        CHECK(host.wire.sent.size() == 1);
        CHECK(host.session.phase() == Phase::Active);
        CHECK(host.session.match().nativeHead() == tick);
        CHECK(host.session.match().context().epoch == epoch);
        CHECK(host.service.next.error == "must not poll during flush");
    }
    SECTION("send failure is surfaced in the same update") {
        host.wire.result = PacketTransport::SendResult::Disconnected;
        CHECK_FALSE(host.session.flush());
        CHECK(host.session.phase() == Phase::Failed);
        CHECK(host.session.failure() == Failure::Match);
        CHECK_FALSE(host.session.flush());
    }
}

TEST_CASE("native guest flush preserves sampled edges and redundant inputs through congestion",
          "[netplay][online-session][early-input-flush]") {
    test::OnlinePair pair{{}, true, 2, 2};
    pair.prepare();
    pair.ready();
    std::vector<RollbackExchange::Message> attempts;
    bool congested = true;
    pair.guestWire.beforeSend = [&](std::span<const u8> bytes, PacketTransport::Delivery delivery) {
        if (const auto packet = RollbackExchange::decode(bytes)) {
            CHECK(delivery == PacketTransport::Delivery::Unreliable);
            attempts.push_back(*packet);
            return congested ? PacketTransport::SendResult::Congested
                             : PacketTransport::SendResult::Sent;
        }
        return PacketTransport::SendResult::Sent;
    };
    const auto firstTick = pair.guestInputs.sampleTick();
    const auto polls = pair.guestWire.polls;
    REQUIRE(firstTick == 2);
    const auto sampled = inputs(pair.guest, pair.guestInputs, true);
    REQUIRE(pair.guestInputs.sample(sampled));
    REQUIRE(pair.hostInputs.sample(inputs(pair.host, pair.hostInputs)));
    pair.guestService.closed = true; // flush must not poll or resample
    REQUIRE(pair.guestSession.flush());
    REQUIRE(pair.guestSession.flush());
    REQUIRE(attempts.size() == 2);
    for (const auto& attempt : attempts) {
        CHECK(InputPacket::encode(attempt.commands) == InputPacket::encode(sampled));
    }
    CHECK(pair.guestWire.polls == polls);
    CHECK(pair.guestInputs.sampleTick() == firstTick + 1);
    CHECK_FALSE(pair.guestInputs.sample(inputs(pair.guest, pair.guestInputs, true)));
    pair.guestService.closed = false;
    congested = false;
    pair.pump();
    std::array<u32, InputCommand::kSeats> presses{};
    for (u64 tick = 1; tick <= 8; ++tick) {
        if (tick > 1) {
            REQUIRE(pair.guestInputs.sample(inputs(pair.guest, pair.guestInputs)));
            REQUIRE(pair.hostInputs.sample(inputs(pair.host, pair.hostInputs)));
            pair.pump();
        }
        const auto host = pair.hostInputs.advance();
        const auto guest = pair.guestInputs.advance();
        REQUIRE(host);
        REQUIRE(guest);
        CHECK(InputPacket::encode(host->commands) == InputPacket::encode(guest->commands));
        CHECK(host->predictedSeats == 0);
        CHECK(guest->predictedSeats == 0);
        for (usize seat = 0; seat < presses.size(); ++seat) {
            presses[seat] += host->commands[seat].pressed(CommandPress::UsePotion) ? 1U : 0U;
        }
        REQUIRE(pair.hostInputs.complete());
        REQUIRE(pair.guestInputs.complete());
    }
    CHECK(presses == std::array<u32, 4>{0, 0, 1, 1});
    pair.guestWire.beforeSend = {};
}

TEST_CASE("native backpressure resends retained input without resampling an edge",
          "[netplay][online-session][early-input-flush]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    usize sends = 0;
    std::vector<InputCommand> recorded;
    pair.guestWire.beforeSend = [&](std::span<const u8> bytes, PacketTransport::Delivery delivery) {
        if (const auto packet = RollbackExchange::decode(bytes)) {
            CHECK(delivery == PacketTransport::Delivery::Unreliable);
            ++sends;
            recorded = packet->commands;
        }
        return PacketTransport::SendResult::Sent;
    };
    const auto sampled = inputs(pair.guest, pair.guestInputs, true);
    REQUIRE(pair.guestInputs.sample(sampled));
    REQUIRE_FALSE(pair.guestInputs.sampleReady());
    const auto pendingTick = pair.guestInputs.sampleTick();
    const auto polls = pair.guestWire.polls;
    for (usize repeat = 0; repeat < 3; ++repeat) {
        CHECK_FALSE(pair.guestInputs.sample(inputs(pair.guest, pair.guestInputs)));
        REQUIRE(pair.guestSession.flush());
        CHECK(pair.guestInputs.sampleTick() == pendingTick);
        CHECK(InputPacket::encode(recorded) == InputPacket::encode(sampled));
    }
    CHECK(sends == 3);
    CHECK(pair.guestWire.polls == polls);
    CHECK(pair.host.nativeHead() == 1);
    pair.guestWire.beforeSend = {};
}

TEST_CASE("an immediate guest input send failure stops the active session",
          "[netplay][online-session][latency][early-input-flush]") {
    const auto result =
        GENERATE(PacketTransport::SendResult::Disconnected, PacketTransport::SendResult::Invalid);
    usize attempts = 0;
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    pair.guestWire.beforeSend = [&](std::span<const u8> bytes, PacketTransport::Delivery) {
        if (RollbackExchange::recognizes(bytes)) {
            ++attempts;
            return result;
        }
        return PacketTransport::SendResult::Sent;
    };
    REQUIRE(pair.guestInputs.sample(inputs(pair.guest, pair.guestInputs)));
    CHECK_FALSE(pair.guestSession.flush());
    CHECK(pair.guestSession.phase() == Phase::Failed);
    CHECK(pair.guestSession.failure() == Failure::Match);
    CHECK(pair.guest.phase() == MatchSession::Phase::Stopped);
    CHECK(pair.guest.stopReason() == (result == PacketTransport::SendResult::Disconnected
                                          ? MatchStop::Disconnected
                                          : MatchStop::TransportFailure));
    CHECK_FALSE(pair.guestSession.flush());
    CHECK(attempts == 1);
    pair.guestWire.beforeSend = {};
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
    REQUIRE(host.timeline.bind(host.session.match()));
    REQUIRE(host.session.match().loaded());
    CHECK_FALSE(host.timeline.advance());
    REQUIRE(guest.timeline.bind(guest.session.match()));
    REQUIRE(guest.session.match().loaded());
    guest.session.update(0);
    host.wire.sent.clear();
    for (const auto& packet : guest.wire.sent) {
        host.wire.message(1, packet.bytes);
    }
    guest.wire.sent.clear();
    host.session.update(0);
    for (const auto& packet : host.wire.sent) {
        guest.wire.message(99, packet.bytes);
    }
    host.wire.sent.clear();
    guest.session.update(0);
    CHECK(host.session.match().phase() == MatchSession::Phase::Running);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Running);
    REQUIRE(guest.timeline.advance());
    REQUIRE(host.timeline.advance());
    CHECK(guest.timeline.complete());
    CHECK(host.timeline.complete());
}

TEST_CASE("the host adopts a startup delay before its contract forms and guests cannot",
          "[netplay][online-session]") {
    Client guest(false);
    CHECK_FALSE(guest.session.adoptStartupDelay(2));
    Client host;
    CHECK_FALSE(host.session.adoptStartupDelay(RollbackInputs::kMaxDelay + 1));
    REQUIRE(host.session.adoptStartupDelay(2));
    auto snapshot = room();
    snapshot.members = {{kGuest, {0}, true}, {kHost, {3}, true}, {kOther, {1}, true}};
    host.publish(snapshot);
    host.connect();
    host.wire.connect(kOther, 2);
    host.session.update(0);
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.nativeContract() != nullptr);
    CHECK(host.session.nativeContract()->rules.startupDelay == 2);
    CHECK_FALSE(host.session.adoptStartupDelay(0));
    CHECK(host.session.match().inputLead() == 2);
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

TEST_CASE("an early host prepare cannot bypass native setup agreement on any roster revision",
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
    CHECK(guest.session.failure() == Failure::Protocol);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
    CHECK_FALSE(guest.session.match().loaded());
    snapshot.started = true;
    guest.publish(snapshot);
    CHECK(guest.session.phase() == Phase::Failed);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
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
        CHECK_FALSE(guest.timeline.advance());
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
    snapshot.members[1].endpointId = kOther;
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
    auto snapshot = room();
    snapshot.started = true;
    guest.publish(snapshot);
    guest.wire.message(99, prepare());
    guest.session.update(0);
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
        invalid.members[1].endpointId = kHost;
    }
    SECTION("changed local seats") {
        invalid.members[0].seats = {2};
    }
    SECTION("changed local player count") {
        invalid.members[0].seats = {0, 2};
    }
    SECTION("changed host") {
        invalid.hostEndpointId = kGuest;
    }
    SECTION("changed room") {
        invalid.code = "BCDEFGHJ";
    }
    SECTION("rollback") {
        invalid.revision = 0;
    }
    SECTION("invalid peer") {
        invalid.members[1].endpointId = "bad";
    }
    SECTION("member changed without revision") {
        invalid.members[1].endpointId = kOther;
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

TEST_CASE("a started guest's lost link drops it and the host session carries on",
          "[netplay][online-session][drop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    REQUIRE(pair.hostInputs.advance());
    REQUIRE(pair.hostInputs.complete());
    pair.hostWire.events.push_back({EventType::Disconnected, 1, {}, {}});
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    REQUIRE(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.dropped(2));
    const auto drops = pair.host.takeDrops();
    REQUIRE(drops.size() == 1);
    CHECK(drops.front().peer == 2);
    REQUIRE(pair.hostInputs.inputs().suspend(2, drops.front().tick));
    // The host confirms its own ticks alone from here on.
    for (u32 step = 0; step < 4; ++step) {
        if (pair.hostInputs.sampleReady()) {
            REQUIRE(pair.hostInputs.sample(inputs(pair.host, pair.hostInputs)));
        }
        REQUIRE(pair.hostInputs.advance());
        REQUIRE(pair.hostInputs.complete());
    }
    CHECK(pair.hostInputs.confirmedTick() >= 4);
    CHECK(pair.hostSession.flush());
    CHECK(pair.hostSession.phase() == Phase::Active);
}

TEST_CASE("a started member's departure reported by the room drops it as its lost link would",
          "[netplay][online-session][drop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    REQUIRE(pair.hostInputs.advance());
    REQUIRE(pair.hostInputs.complete());
    // The room's word comes first, while the guest's link is still up.
    pair.room.members.pop_back();
    ++pair.room.revision;
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    REQUIRE(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.dropped(2));
    REQUIRE(pair.hostSession.room());
    CHECK(pair.hostSession.room()->members.size() == 1);
    // The link closing after that changes nothing.
    pair.hostWire.events.push_back({EventType::Disconnected, 1, {}, {}});
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    CHECK(pair.host.phase() == MatchSession::Phase::Running);
    const auto drops = pair.host.takeDrops();
    REQUIRE(drops.size() == 1);
    CHECK(drops.front().peer == 2);
    REQUIRE(pair.hostInputs.inputs().suspend(2, drops.front().tick));
    CHECK(pair.hostSession.flush());
    CHECK(pair.hostSession.phase() == Phase::Active);
}

TEST_CASE("a guest that leaves on its own is dropped, its link closing afterwards changing nothing",
          "[netplay][online-session][drop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    REQUIRE(pair.hostInputs.advance());
    REQUIRE(pair.hostInputs.complete());
    // The guest's Stop reaches the host over the still-open link.
    pair.guestSession.leave();
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    REQUIRE(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.dropped(2));
    // Then the link goes, as the leaving guest closes it.
    pair.hostWire.events.push_back({EventType::Disconnected, 1, {}, {}});
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    CHECK(pair.host.phase() == MatchSession::Phase::Running);
    const auto drops = pair.host.takeDrops();
    REQUIRE(drops.size() == 1);
    CHECK(drops.front().peer == 2);
    CHECK(pair.hostSession.flush());
}

TEST_CASE("what a guest sent before the host dropped it is ignored when it arrives",
          "[netplay][online-session][drop]") {
    const bool leaving = GENERATE(false, true);
    CAPTURE(leaving);
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    if (leaving) {
        pair.guestSession.leave(); // its Stop is on the way
    } else {
        REQUIRE(pair.guestInputs.sample(inputs(pair.guest, pair.guestInputs)));
        REQUIRE(pair.guestSession.flush()); // its input is on the way
    }
    REQUIRE_FALSE(pair.hostWire.events.empty());
    // The stall deadline drops the guest before the host reads what it sent.
    REQUIRE(pair.host.drop(2));
    pair.hostSession.update(0);
    CHECK(pair.hostSession.failure() == Failure::None);
    CHECK(pair.hostSession.phase() == Phase::Active);
    CHECK(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.takeDrops().size() == 1);
    CHECK(pair.hostSession.flush());
}

TEST_CASE("online state fails closed after a started room changes or its service fails",
          "[netplay][online-session]") {
    Client host;
    host.publish(room());
    host.connect();
    auto snapshot = room();
    snapshot.started = true;
    host.publish(snapshot);
    REQUIRE(host.session.match().prepare(7));
    SECTION("service removal before the first stage begins") {
        snapshot.members.pop_back();
        ++snapshot.revision;
        host.publish(snapshot);
        CHECK(host.session.failure() == Failure::RosterChanged);
    }
    SECTION("service seat change") {
        snapshot.members.back().seats = {2};
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
    CHECK_FALSE(host.timeline.advance());
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
    CHECK_FALSE(host.session.failureDetail().empty());
    CHECK(host.service.leaves == 1);
}

TEST_CASE("a guest whose terminal certificate never comes is dropped and the host's exit agrees",
          "[netplay][online-session][drop]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    REQUIRE(pair.hostInputs.advance());
    REQUIRE(pair.hostInputs.complete());
    const auto epoch = pair.host.context().epoch;
    const auto now = MatchSession::Clock::now();
    REQUIRE(pair.host.observeNativeTerminal(epoch, 1, NativeAgreement::Digest{}, now));
    CHECK_FALSE(pair.host.nativeTerminalAgreed(epoch, 1));
    pair.host.update(0, now + NativeAgreement::kTimeout);
    CHECK(pair.host.phase() == MatchSession::Phase::Running);
    CHECK(pair.host.dropped(2));
    CHECK(pair.host.nativeTerminalAgreed(epoch, 1));
}

TEST_CASE("online failure preserves terminal agreement diagnostics through teardown",
          "[netplay][online-session][session-recovery]") {
    test::OnlinePair pair;
    pair.prepare();
    pair.ready();
    REQUIRE(pair.hostInputs.advance());
    REQUIRE(pair.hostInputs.complete());
    const auto epoch = pair.host.context().epoch;
    const auto now = MatchSession::Clock::now();
    const NativeAgreement::Digest first{};
    REQUIRE(pair.host.observeNativeTerminal(epoch, 1, first, now));
    CHECK_FALSE(pair.host.completeNativeBoundary(epoch, 1));
    auto changed = first;
    changed[0] = 1;
    CHECK_FALSE(pair.host.observeNativeTerminal(epoch, 1, changed, now));
    const std::string expected = "conflicting digest";
    REQUIRE(pair.host.phase() == MatchSession::Phase::Stopped);
    CHECK_FALSE(pair.host.nativeTerminalAgreed(epoch, 1));
    CHECK_FALSE(pair.host.completeNativeBoundary(epoch, 1));
    const auto detail = pair.host.failureDetail();
    CHECK(detail.find(expected) != std::string::npos);
    CHECK(detail.find("tick 1") != std::string::npos);
    CHECK_FALSE(pair.hostSession.flush());
    CHECK(pair.hostSession.phase() == Phase::Failed);
    CHECK(pair.hostSession.failure() == Failure::Match);
    CHECK(pair.hostSession.failureDetail() == detail);
    pair.hostSession.leave();
    pair.hostSession.update(60);
    CHECK_FALSE(pair.hostSession.flush());
    CHECK(pair.hostSession.failureDetail() == detail);
    CHECK_FALSE(pair.hostInputs.advance());
    CHECK_FALSE(pair.hostInputs.publish());

    // Rejoining is a new room/transport/session, not revival of failed history.
    test::OnlinePair next;
    next.prepare();
    next.ready();
    CHECK(next.hostSession.failure() == Failure::None);
    CHECK(next.hostSession.failureDetail().empty());
    CHECK(next.host.failureDetail().empty());
    CHECK(next.host.nativeConfirmed() == 0);
    CHECK_FALSE(next.host.nativeTerminalAgreed(next.host.context().epoch, 1));
    REQUIRE(next.hostInputs.advance());
    REQUIRE(next.hostInputs.complete());
    CHECK(next.host.nativeConfirmed() == 1);
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
    CHECK_FALSE(session.ready());
    REQUIRE(session.configureNative(nativeIdentity(), OnlineSession::NativeRules{}));
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
TEST_CASE("early character relay survives a delayed room start before native setup agreement",
          "[netplay][online-session]") {
    Client guest(false);
    guest.publish(room());
    guest.connect();
    guest.wire.message(99, partyPacket(0, 0));
    guest.wire.message(99, partyPacket(0, 1));
    guest.wire.message(99, partyPacket(1));
    guest.session.update(0);
    CHECK(guest.session.party() == nullptr);
    CHECK(guest.session.match().phase() == MatchSession::Phase::Offline);
    auto snapshot = room();
    snapshot.started = true;
    guest.publish(snapshot);
    REQUIRE(guest.session.phase() == Phase::Active);
    REQUIRE(guest.session.party());
    CHECK((*guest.session.party())[1]->name == "TEST");
    CHECK(guest.session.match().phase() == MatchSession::Phase::Lobby);
    guest.wire.message(99, prepare());
    guest.session.update(0);
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
    CHECK_FALSE(host.timeline.advance());
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

TEST_CASE(
    "a guest dials the members above it, accepts the ones below, and keeps going without them",
    "[netplay][online-session][direct-link]") {
    const std::string below(32, '0');
    Client guest(false);
    auto snapshot = room();
    snapshot.members = {
        {kHost, {0}, false}, {kGuest, {1}, false}, {kOther, {2}, false}, {below, {3}, false}};
    guest.publish(snapshot);
    CHECK(guest.wire.authorized == std::vector<std::string>{kHost, kOther, below});
    REQUIRE(guest.wire.peers.at(99) == kHost);
    REQUIRE(guest.wire.peers.at(98) == kOther);
    CHECK(std::ranges::none_of(guest.wire.peers,
                               [&](const auto& row) { return row.second == below; }));
    guest.wire.incoming.push_back({EventType::Connected, 99, {}, {}});
    guest.session.update(0);
    CHECK(guest.session.connected()); // the star is all that is required
    CHECK(guest.session.directLinks() == 0);
    // A dial that fails changes nothing, and the next roster update tries again.
    guest.wire.incoming.push_back({EventType::Disconnected, 98, {}, {}});
    guest.wire.peers.erase(98); // the transport forgets a connection that failed
    guest.session.update(0);
    CHECK(guest.session.failure() == Failure::None);
    CHECK(guest.session.phase() == Phase::Lobby);
    snapshot.members[2].ready = true;
    guest.publish(snapshot);
    REQUIRE(guest.wire.peers.at(97) == kOther);
    guest.wire.incoming.push_back({EventType::Connected, 97, {}, {}});
    guest.session.update(0);
    CHECK(guest.session.directLinks() == 1);
    // The member below dials us; a stranger is refused.
    guest.wire.connect(below, 50);
    guest.session.update(0);
    CHECK(guest.session.directLinks() == 2);
    guest.wire.connect(std::string(32, 'z'), 51);
    guest.session.update(0);
    CHECK(guest.session.directLinks() == 2);
    CHECK(std::ranges::find(guest.wire.closed, Connection{51}) != guest.wire.closed.end());
    // Losing a direct link is no failure.
    guest.wire.incoming.push_back({EventType::Disconnected, 50, {}, {}});
    guest.session.update(0);
    CHECK(guest.session.directLinks() == 1);
    CHECK(guest.session.failure() == Failure::None);
    CHECK(guest.session.phase() == Phase::Lobby);
    CHECK(guest.session.connected());
}
