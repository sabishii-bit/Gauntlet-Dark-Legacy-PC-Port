#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/platform/Input.h"

#include "TestSupport.h"
#include "game/camera/CameraMovementLimit.h"
#include "game/netplay/RollbackExchange.h"
#include "game/party/PartyMotion.h"
#include "game/rollback/RollbackSessionHarness.h"
#include "game/rollback/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = MatchSession::Phase;
using Admission = MatchSession::Admission;

struct Wire final : PacketTransport {
    struct Packet {
        Connection connection;
        std::vector<u8> bytes;
        Delivery delivery;
    };
    SendResult result = SendResult::Sent;
    Connection failingConnection = 0; // zero applies result to every connection
    usize budget = std::numeric_limits<usize>::max();
    std::vector<Packet> sent;
    std::optional<Statistics> stats;
    mutable usize statisticsQueries = 0;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        const auto outcome =
            failingConnection == 0 || connection == failingConnection ? result : SendResult::Sent;
        if (outcome == SendResult::Sent) {
            if (budget == 0) {
                return SendResult::Congested;
            }
            --budget;
            sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        }
        return outcome;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        ++statisticsQueries;
        return stats;
    }
    void close(Connection /*connection*/) override {}
};

struct Room {
    MatchOwners owners;
    std::array<MatchSession, 5> sessions;
    std::array<Wire, 5> wires;
    std::array<test::RollbackSessionHarness, 5> inputs;
    std::array<u64, 5> bound{};
    std::array<u64, 5> completed{};
    explicit Room(MatchOwners roster = {1, 2, 3, 4}) : owners(roster) {
        std::vector<MatchLink> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, static_cast<u64>(peer - 1)});
                const std::array server{MatchLink{1, 99}};
                REQUIRE(sessions[peer].open(peer, owners, server, 0));
            }
        }
        REQUIRE(sessions[1].open(1, owners, links, 0));
    }
    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
    bool loaded(u8 peer) {
        auto& session = sessions[peer];
        if (session.phase() != Phase::Loading) {
            return false;
        }
        if (session.context().transition != MatchTransition::Shop &&
            bound[peer] != session.context().epoch) {
            if (!inputs[peer].bind(session)) {
                return false;
            }
            bound[peer] = session.context().epoch;
        }
        return session.loaded();
    }
    bool sample(u8 peer, const SessionInputs::Frame& local = {}) {
        std::vector<InputCommand> commands;
        const auto& context = sessions[peer].context();
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] == peer) {
                commands.push_back(SessionInputs::command(
                    local[commands.size()], context.epoch, inputs[peer].sampleTick(),
                    context.grants[seat], static_cast<u8>(seat)));
            }
        }
        return inputs[peer].sample(commands);
    }
    std::optional<SessionInputs::Frame> advance(u8 peer) {
        const auto frame = inputs[peer].advance();
        if (!frame) {
            return std::nullopt;
        }
        SessionInputs::Frame result;
        for (usize seat = 0; seat < result.size(); ++seat) {
            result[seat] = SessionInputs::playInput(frame->commands[seat]);
        }
        return result;
    }
    MatchParty party() const {
        MatchParty result;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                CharacterProfile profile;
                profile.name = "TEST";
                profile.character = static_cast<s32>(seat);
                profile.gold = static_cast<s32>(seat) * 100;
                result[seat] = profile;
            }
        }
        return result;
    }
    void pump(MatchSession::Time now = MatchSession::Clock::now()) {
        for (u8 pass = 0; pass < 5; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (!present(peer)) {
                    continue;
                }
                sessions[peer].flush(wires[peer], now);
                const auto sent = std::move(wires[peer].sent);
                wires[peer].sent.clear();
                for (const auto& packet : sent) {
                    const auto target = peer == 1 ? packet.connection + 1 : 1;
                    const auto sender = peer == 1 ? 99 : peer - 1;
                    const auto admitted = sessions[target].receive(sender, packet.bytes, now);
                    REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
                    // This test adapter has already executed the same terminal
                    // frame. Acknowledgment is explicit, not receipt-driven advancement.
                    if (const auto* pending = sessions[target].pendingNativeContext()) {
                        REQUIRE(pending->sourceTick == completed[target]);
                        REQUIRE(sessions[target].completeNativeBoundary(
                            sessions[target].context().epoch, completed[target]));
                        completed[target] = 0;
                    }
                }
            }
        }
    }
    void completeOne() {
        const u64 tick = inputs[1].tick();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sample(peer));
            }
        }
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(inputs[peer].advance());
                REQUIRE(inputs[peer].submitBoundaryDigest());
                completed[peer] = tick;
            }
        }
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(inputs[peer].completeBoundary());
            }
        }
    }
    void ready(MatchTransition transition = MatchTransition::Start, u32 scene = 7,
               bool challengeWon = false) {
        const auto profiles = party();
        const bool checkpointed = transition == MatchTransition::Travel ||
                                  transition == MatchTransition::SecretEnter ||
                                  transition == MatchTransition::SecretReturn;
        if (checkpointed) {
            completeOne();
        }
        REQUIRE(sessions[1].prepare(scene, transition, checkpointed ? &profiles : nullptr, nullptr,
                                    challengeWon));
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Loading);
                REQUIRE(loaded(peer));
            }
        }
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Running);
            }
        }
    }
};
std::vector<u8> message(const MatchSession& session, MatchControlKind kind,
                        MatchStop stop = MatchStop::None, u8 votes = 0) {
    const auto bytes =
        MatchControlPacket::encode({kind, stop, session.context(), SeatSet::fromBits(votes)});
    REQUIRE(bytes);
    return *bytes;
}
std::vector<u8> consoleMessageFrom(const Wire& wire, u8 kind) {
    const auto packet = std::ranges::find_if(wire.sent, [kind](const auto& sent) {
        return sent.bytes.size() >= 6 && sent.bytes[0] == 'G' && sent.bytes[1] == 'D' &&
               sent.bytes[2] == 'L' && sent.bytes[3] == 'C' && sent.bytes[5] == kind;
    });
    REQUIRE(packet != wire.sent.end());
    CHECK(packet->delivery == PacketTransport::Delivery::Reliable);
    return packet->bytes;
}

TEST_CASE("console permissions are host-only retained under congestion and default off",
          "[netplay][match-session][console-input]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    CHECK_FALSE(host.consolePermissions());
    CHECK_FALSE(guest.consolePermissions());
    CHECK_FALSE(guest.setConsolePermissions(true));
    CHECK_FALSE(host.requestConsole({0, "god"})); // not in gameplay yet
    room.ready();
    CHECK_FALSE(guest.requestConsole({1, "god"}));
    REQUIRE(host.requestConsole({0, "god"}));
    CHECK(host.takeConsoleRequest() == std::optional{ConsoleAction{0, "god"}});
    REQUIRE(host.setConsolePermissions(true));
    room.wires[1].result = PacketTransport::SendResult::Congested;
    host.flush(room.wires[1]);
    CHECK_FALSE(guest.consolePermissions());
    CHECK(host.queuedControls() != 0);
    room.wires[1].result = PacketTransport::SendResult::Sent;
    host.flush(room.wires[1]);
    const auto permission = consoleMessageFrom(room.wires[1], 0);
    CHECK(host.receive(1, permission) == Admission::WrongPeer); // forged host privilege
    CHECK(guest.receive(99, permission) == Admission::Accepted);
    CHECK(guest.receive(99, permission) == Admission::Stale);
    CHECK(guest.consolePermissions());
    room.wires[1].sent.clear();
    REQUIRE(guest.requestConsole({1, "get gold 50"}));
    room.pump();
    CHECK(host.takeConsoleRequest() == std::optional{ConsoleAction{1, "get gold 50"}});
    CHECK_FALSE(host.takeConsoleRequest());
    room.ready(MatchTransition::Travel, 8);
    CHECK(host.consolePermissions());
    CHECK(guest.consolePermissions());
    host.clear();
    guest.clear();
    CHECK_FALSE(host.consolePermissions());
    CHECK_FALSE(guest.consolePermissions());
}

TEST_CASE("console request admission checks ownership duplicates revocation and stale epochs",
          "[netplay][match-session][console-input]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.setConsolePermissions(true));
    room.pump();
    CHECK_FALSE(guest.requestConsole({0, "god"}));
    REQUIRE(guest.requestConsole({1, "god"}));
    guest.flush(room.wires[2]);
    const auto request = consoleMessageFrom(room.wires[2], 1);
    room.wires[2].sent.clear();
    for (usize cut = 0; cut < request.size(); ++cut) {
        CHECK(host.receive(1, std::span(request).first(cut)) == Admission::Invalid);
    }
    auto forged = request;
    forged[7] = 0; // another peer's player
    CHECK(host.receive(1, forged) == Admission::WrongPeer);
    forged = request;
    forged[32] ^= 1; // stale seat grant
    CHECK(host.receive(1, forged) == Admission::WrongPeer);
    CHECK(guest.receive(99, request) == Admission::WrongPeer);
    CHECK(host.receive(1, request) == Admission::Accepted);
    CHECK(host.receive(1, request) == Admission::Stale);
    REQUIRE(host.requestConsole({0, "notarget"}));
    REQUIRE(host.setConsolePermissions(false));
    CHECK(host.takeConsoleRequest() == std::optional{ConsoleAction{0, "notarget"}});
    CHECK_FALSE(host.takeConsoleRequest()); // guest's not-yet-admitted command was revoked
    REQUIRE(host.setConsolePermissions(true));
    CHECK(host.receive(1, request) == Admission::Stale);
    room.pump();
    REQUIRE(guest.requestConsole({1, "get gold 1"}));
    guest.flush(room.wires[2]);
    const auto oldEpoch = consoleMessageFrom(room.wires[2], 1);
    room.wires[2].sent.clear();
    room.ready(MatchTransition::Travel, 8);
    CHECK(host.receive(1, oldEpoch) == Admission::Stale);
    CHECK_FALSE(host.takeConsoleRequest());
}

TEST_CASE("console host requests are bounded and discarded when entering a load barrier",
          "[netplay][match-session][console-input]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    for (usize i = 0; i < 32; ++i) {
        REQUIRE(host.requestConsole({0, "god"}));
    }
    CHECK_FALSE(host.requestConsole({0, "god"}));
    room.completeOne();
    const auto party = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK_FALSE(host.requestConsole({0, "god"}));
    CHECK_FALSE(host.takeConsoleRequest());
}

TEST_CASE("match control has an exact bounded versioned encoding", "[netplay][match-session]") {
    Room room;
    REQUIRE(room.sessions[1].prepare(7));
    for (const u8 kind : {u8{0}, u8{1}, u8{2}, u8{5}, u8{6}}) {
        const auto stop = kind == 5 ? MatchStop::Left : MatchStop::None;
        const u8 votes = kind == 6 ? 1 : 0;
        const auto bytes =
            message(room.sessions[1], static_cast<MatchControlKind>(kind), stop, votes);
        REQUIRE(bytes.size() == MatchControlPacket::kBytes);
        const auto decoded = MatchControlPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->context == room.sessions[1].context());
        CHECK(decoded->kind == static_cast<MatchControlKind>(kind));
        CHECK(decoded->stop == stop);
        CHECK(decoded->movieSkipVotes.bits() == votes);
        CHECK(bytes[4] == MatchControlPacket::kVersion);
        for (usize size = 0; size < bytes.size(); ++size) {
            CHECK_FALSE(MatchControlPacket::decode(std::span(bytes).first(size)));
        }
        auto trailing = bytes;
        trailing.push_back(0);
        CHECK_FALSE(MatchControlPacket::decode(trailing));
        for (const usize index : {0U, 4U, 5U, 6U, 7U, 20U, 22U, 23U, 24U, 44U}) {
            auto corrupt = bytes;
            corrupt[index] = 255;
            CHECK_FALSE(MatchControlPacket::decode(corrupt));
        }
    }
    auto context = room.sessions[1].context();
    SECTION("missing seat grant") {
        context.grants[0] = 0;
    }
    SECTION("grant on empty seat") {
        context.owners[0] = 0;
    }
    SECTION("no host") {
        context.owners.fill(2);
    }
    SECTION("invalid scene") {
        context.scene = 65536;
    }
    SECTION("zero epoch") {
        context.epoch = 0;
    }
    CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context}));
}

TEST_CASE("match control packets keep their byte layout", "[netplay][match-session]") {
    MatchControl control;
    control.kind = MatchControlKind::Drop;
    control.resyncTick = 0x0123456789abcdefULL;
    control.resyncPeers = PeerSet::fromBits(0b01000);
    auto& context = control.context;
    context.epoch = 0x1122334455667788ULL;
    context.scene = 0x1234;
    context.inputLead = 2;
    context.transition = MatchTransition::Travel;
    context.owners = {1, 2, 3, 0};
    context.grants = {0x01020304, 0x05060708, 0x090a0b0c, 0};
    context.sourceEpoch = context.epoch - 1;
    context.sourceTick = 0xa1b2c3d4e5f60718ULL;
    const auto bytes = MatchControlPacket::encode(control);
    REQUIRE(bytes);
    constexpr std::string_view kControl = "47444c54"         // magic
                                          "08090000"         // version, drop, stop, votes
                                          "8877665544332211" // epoch
                                          "34120000"         // scene
                                          "3c000201"         // tick rate, input lead, travel
                                          "01020300"         // owners
                                          "04030201080706050c0b0a0900000000" // grants
                                          "00080000"          // challenge won, resync peers
                                          "8777665544332211"  // source epoch
                                          "1807f6e5d4c3b2a1"  // source tick
                                          "efcdab8967452301"; // resync tick
    CHECK(test::hexBytes(*bytes) == kControl);
    const auto decoded = MatchControlPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->context == context);
    CHECK(decoded->resyncTick == control.resyncTick);
    CHECK(decoded->resyncPeers == control.resyncPeers);
    CHECK(MatchControlPacket::encode(*decoded) == bytes);
}

TEST_CASE("match control seat votes and peer sets keep their byte layout",
          "[netplay][match-session]") {
    MatchContext context;
    context.epoch = 3;
    context.scene = 0x1234;
    context.owners = {1, 2, 1, 0};
    context.grants = {0x11, 0x22, 0x33, 0};
    MatchControl vote;
    vote.kind = MatchControlKind::MovieSkipVote;
    vote.context = context;
    vote.movieSkipVotes = SeatSet::fromBits(0b0101);
    const auto voteBytes = MatchControlPacket::encode(vote);
    REQUIRE(voteBytes);
    constexpr std::string_view kVote = "47444c54"                         // magic
                                       "08060005"                         // version, vote, votes
                                       "0300000000000000"                 // epoch
                                       "34120000"                         // scene
                                       "3c000100"                         // tick rate, lead, start
                                       "01020100"                         // owners
                                       "11000000220000003300000000000000" // grants
                                       "00000000"          // challenge won, resync peers
                                       "0000000000000000"  // source epoch
                                       "0000000000000000"  // source tick
                                       "0000000000000000"; // resync tick
    CHECK(test::hexBytes(*voteBytes) == kVote);
    const auto decodedVote = MatchControlPacket::decode(*voteBytes);
    REQUIRE(decodedVote);
    CHECK(decodedVote->movieSkipVotes == vote.movieSkipVotes);

    MatchControl resync;
    resync.kind = MatchControlKind::Resync;
    resync.context = context;
    resync.resyncTick = 60;
    resync.resyncPeers = PeerSet::fromBits(0b01100);
    const auto resyncBytes = MatchControlPacket::encode(resync);
    REQUIRE(resyncBytes);
    constexpr std::string_view kResync = "47444c54"                         // magic
                                         "08070000"                         // version, resync
                                         "0300000000000000"                 // epoch
                                         "34120000"                         // scene
                                         "3c000100"                         // tick rate, lead
                                         "01020100"                         // owners
                                         "11000000220000003300000000000000" // grants
                                         "000c0000"          // challenge won, peers two, three
                                         "0000000000000000"  // source epoch
                                         "0000000000000000"  // source tick
                                         "3c00000000000000"; // resync tick
    CHECK(test::hexBytes(*resyncBytes) == kResync);
    const auto decodedResync = MatchControlPacket::decode(*resyncBytes);
    REQUIRE(decodedResync);
    CHECK(decodedResync->resyncPeers == resync.resyncPeers);
}

TEST_CASE("console packets keep their byte layout", "[netplay][match-session][console-input]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.setConsolePermissions(true));
    host.flush(room.wires[1]);
    const auto permission = consoleMessageFrom(room.wires[1], 0);
    constexpr std::string_view kPermission = "47444c43"         // magic
                                             "01000100"         // version, permission, on, seat
                                             "0000000000000000" // epoch
                                             "0100000000000000" // revision
                                             "0000000000000000" // request
                                             "00000000"         // grant
                                             "00";              // line length
    CHECK(test::hexBytes(permission) == kPermission);
    REQUIRE(guest.receive(99, permission) == Admission::Accepted);
    room.wires[1].sent.clear();
    REQUIRE(guest.requestConsole({1, "get gold 50"}));
    guest.flush(room.wires[2]);
    const auto request = consoleMessageFrom(room.wires[2], 1);
    constexpr std::string_view kRequest = "47444c43"                // magic
                                          "01010001"                // version, request, off, seat
                                          "0100000000000000"        // epoch
                                          "0100000000000000"        // revision
                                          "0100000000000000"        // request
                                          "01000000"                // grant
                                          "0b"                      // line length
                                          "67657420676f6c64203530"; // "get gold 50"
    CHECK(test::hexBytes(request) == kRequest);
    CHECK(host.receive(1, request) == Admission::Accepted);
    CHECK(host.takeConsoleRequest() == std::optional{ConsoleAction{1, "get gold 50"}});
}

TEST_CASE("secret result flags are bounded host context and secret entries have no movie votes",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.sessions[1].prepare(7));
    auto context = room.sessions[1].context();
    for (const auto transition :
         {MatchTransition::Start, MatchTransition::Travel, MatchTransition::Resume,
          MatchTransition::Shop, MatchTransition::SecretEnter, MatchTransition::SecretReturn}) {
        context.transition = transition;
        context.epoch = transition == MatchTransition::Start ? 1 : 2;
        context.sourceEpoch = transition == MatchTransition::Start ? 0 : 1;
        context.sourceTick = transition == MatchTransition::Start ? 0 : 1;
        context.challengeWon = false;
        CHECK(MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context})
                  .has_value() == (transition != MatchTransition::Resume));
        context.challengeWon = true;
        const auto encoded =
            MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
        CHECK(encoded.has_value() == (transition == MatchTransition::SecretReturn));
        context.challengeWon = false;
        const auto vote = MatchControlPacket::encode(
            {MatchControlKind::MovieSkipVote, MatchStop::None, context, SeatSet::of(0)});
        CHECK(vote.has_value() ==
              (transition == MatchTransition::Start || transition == MatchTransition::Travel));
    }
    context.transition = MatchTransition::SecretReturn;
    context.challengeWon = true;
    const auto encoded =
        MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
    REQUIRE(encoded);
    REQUIRE(encoded->size() == MatchControlPacket::kBytes);
    CHECK((*encoded)[44] == 1);
    REQUIRE(MatchControlPacket::decode(*encoded));
    CHECK(MatchControlPacket::decode(*encoded)->context.challengeWon);
    auto invalid = *encoded;
    invalid[4] = 3;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    invalid = *encoded;
    invalid[44] = 2;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    invalid = *encoded;
    invalid[45] = 1;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
}

TEST_CASE("secret enter and return form fresh authenticated checkpoint epochs without movies",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 2, 1});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto startEpoch = host.context().epoch;
    const auto party = room.party();
    CHECK_FALSE(host.prepare(8, MatchTransition::SecretEnter, &party, nullptr, true));
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party, nullptr, true));
    CHECK(host.context().epoch == startEpoch);
    room.ready(MatchTransition::SecretEnter, 8);
    CHECK(host.context().epoch == startEpoch + 1);
    CHECK(guest.context() == host.context());
    CHECK_FALSE(host.context().challengeWon);
    CHECK(host.travelParty());
    CHECK(guest.travelParty());
    const bool won = GENERATE(false, true);
    room.completeOne();
    REQUIRE(host.prepare(7, MatchTransition::SecretReturn, &party, nullptr, won));
    room.pump();
    CHECK_FALSE(host.requestMovieSkip(0));
    CHECK_FALSE(guest.requestMovieSkip(1));
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    CHECK(guest.context().challengeWon == won);
    CHECK(guest.context().epoch == startEpoch + 2);
    auto forged = guest.context();
    forged.challengeWon = !won;
    const auto forgedPrepare =
        MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, forged});
    REQUIRE(forgedPrepare);
    CHECK(host.receive(1, *forgedPrepare) == Admission::WrongPeer);
    const auto forgedReady =
        MatchControlPacket::encode({MatchControlKind::Ready, MatchStop::None, forged});
    REQUIRE(forgedReady);
    CHECK(host.receive(1, *forgedReady) == Admission::Stale);
    CHECK(host.context().challengeWon == won);
    REQUIRE(room.loaded(1));
    REQUIRE(room.loaded(2));
    room.pump();
    CHECK(host.phase() == Phase::Running);
    CHECK(guest.phase() == Phase::Running);
}

TEST_CASE("secret transitions require running gameplay and cannot leave a shop",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto party = room.party();
    CHECK_FALSE(host.prepare(8, MatchTransition::SecretEnter, &party));
    CHECK_FALSE(host.prepare(7, MatchTransition::SecretReturn, &party));
    room.ready();
    room.completeOne();
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    setup.maxima = {1000, 100, 1000};
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    room.pump();
    REQUIRE(room.loaded(1));
    REQUIRE(room.loaded(2));
    room.pump();
    for (const auto transition : {MatchTransition::SecretEnter, MatchTransition::SecretReturn}) {
        CHECK_FALSE(host.prepare(8, transition, &party));
        auto context = host.context();
        context.sourceEpoch = context.epoch;
        ++context.epoch;
        context.sourceTick = 0; // Shop has no native scene frame.
        context.transition = transition;
        const auto packet =
            MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
        REQUIRE(packet);
        CHECK(guest.receive(99, *packet) == Admission::WrongPhase);
    }
    CHECK(guest.context().transition == MatchTransition::Shop);
}

TEST_CASE("movie skipping requires every occupied seat not just every machine",
          "[netplay][match-session][movie-skip]") {
    Room room({1, 1, 2, 2});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    CHECK_FALSE(host.requestMovieSkip(0));
    REQUIRE(host.prepare(7));
    room.pump();
    CHECK_FALSE(guest.requestMovieSkip(0));
    CHECK_FALSE(host.requestMovieSkip(4));
    REQUIRE(host.requestMovieSkip(0));
    REQUIRE(guest.requestMovieSkip(2));
    room.pump();
    CHECK(host.movieSkipVotes().bits() == 5);
    CHECK(guest.movieSkipVotes().bits() == 5);
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    REQUIRE(host.requestMovieSkip(1));
    room.pump();
    for (s32 duplicate = 0; duplicate < 30; ++duplicate) {
        REQUIRE(guest.requestMovieSkip(2));
        REQUIRE(host.requestMovieSkip(0));
    }
    CHECK(host.queuedControls() == 0);
    CHECK(guest.queuedControls() == 0);
    CHECK_FALSE(host.movieSkipReady());
    REQUIRE(guest.requestMovieSkip(3));
    CHECK_FALSE(guest.movieSkipReady()); // A local vote is not host approval.
    room.wires[1].result = PacketTransport::SendResult::Congested;
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK(guest.movieSkipReady());
    CHECK(room.inputs[1].tick() == 1);
    REQUIRE(room.loaded(1));
    REQUIRE(room.loaded(2));
    room.pump();
    const auto oldVote = message(guest, MatchControlKind::MovieSkipVote, MatchStop::None, 8);
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    room.completeOne();
    const auto profiles = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
    room.pump();
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    CHECK(host.movieSkipVotes().empty());
    REQUIRE(guest.requestMovieSkip(2));
    room.pump();
    CHECK(host.movieSkipVotes().bits() == 4);
}

TEST_CASE("movie skip masks cannot vote for empty seats or another peer's players",
          "[netplay][match-session][movie-skip]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.prepare(7));
    room.pump();
    const auto forged = message(guest, MatchControlKind::MovieSkipVote, MatchStop::None, 3);
    CHECK(host.receive(1, forged) == Admission::WrongPeer);
    CHECK(host.movieSkipVotes().empty());
    CHECK_FALSE(host.requestMovieSkip(2));
    for (const u8 mask : std::array<u8, 4>{0, 4, 16, 255}) {
        CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::MovieSkipVote, MatchStop::None,
                                                host.context(), SeatSet::fromBits(mask)}));
    }
    CHECK_FALSE(MatchControlPacket::encode(
        {MatchControlKind::Ready, MatchStop::None, host.context(), SeatSet::of(0)}));
    REQUIRE(host.requestMovieSkip(0));
    REQUIRE(guest.requestMovieSkip(1));
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK(guest.movieSkipReady());
}

TEST_CASE("match admission separates room peer identities from connection handles",
          "[netplay][match-session]") {
    MatchSession session;
    const MatchOwners owners{1, 2, 3, 0};
    const std::array duplicate{MatchLink{2, 1}, MatchLink{3, 1}};
    CHECK_FALSE(session.open(1, owners, duplicate));
    const std::array missing{MatchLink{2, 1}};
    CHECK_FALSE(session.open(1, owners, missing));
    CHECK_FALSE(session.open(4, owners, missing));
    CHECK(session.phase() == Phase::Offline);
    const std::array valid{MatchLink{2, 1}, MatchLink{3, 2}};
    REQUIRE(session.open(1, owners, valid));
    CHECK_FALSE(session.open(1, owners, valid));
    CHECK_FALSE(session.publishNativeInputs({}, 1));
    CHECK_FALSE(session.loaded());
    CHECK_FALSE(session.requestPause());
    REQUIRE(session.prepare(1));
    const auto control = message(session, MatchControlKind::Commit);
    CHECK(session.receive(1, control) == Admission::WrongPeer);
    CHECK(session.receive(99, control) == Admission::WrongPeer);
    session.leave();
    CHECK_FALSE(session.prepare(1));
    session.clear();
    CHECK(session.phase() == Phase::Offline);
    CHECK(session.open(1, owners, valid));
}

TEST_CASE("travel and secret transitions wait for every profile and reject forged progression",
          "[netplay][match-session][party-checkpoint][secret-transition]") {
    const auto transition = GENERATE(MatchTransition::Travel, MatchTransition::SecretEnter,
                                     MatchTransition::SecretReturn);
    const bool won = transition == MatchTransition::SecretReturn;
    Room room({1, 2, 2, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    room.completeOne();
    const auto oldContext = host.context();
    auto party = room.party();
    party[1]->gold = 7654;
    party[1]->progress.inventory.keys = 7;
    CHECK_FALSE(host.prepare(8, transition));
    auto missing = party;
    missing[2].reset();
    CHECK_FALSE(host.prepare(8, transition, &missing, nullptr, won));
    CHECK(host.context() == oldContext);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.prepare(8, transition, &party, nullptr, won));
    REQUIRE(host.travelParty());
    REQUIRE(room.loaded(1));
    auto& wire = room.wires[1];
    wire.budget = 1; // Prepare arrives, then each profile in a separate batch.
    host.flush(wire);
    REQUIRE(wire.sent.size() == 1);
    REQUIRE(guest.receive(99, wire.sent.back().bytes) == Admission::Accepted);
    REQUIRE(guest.pendingNativeContext());
    REQUIRE(guest.completeNativeBoundary(guest.context().epoch, room.completed[2]));
    room.completed[2] = 0;
    CHECK(guest.context().transition == transition);
    CHECK(guest.context().challengeWon == won);
    wire.sent.clear();
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(room.loaded(2));
    CHECK_FALSE(room.inputs[1].advance());

    PartyCheckpoint forged{host.context().epoch, 0, *party[0]};
    auto encoded = PartyCheckpointPacket::encode(forged);
    REQUIRE(encoded);
    CHECK(host.receive(1, *encoded) == Admission::WrongPeer);
    CHECK(guest.receive(1, *encoded) == Admission::WrongPeer);
    ++forged.epoch;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Invalid);
    forged.epoch = oldContext.epoch;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Stale);
    forged.epoch = host.context().epoch;
    forged.seat = 3;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Invalid);

    for (usize seat = 0; seat < 3; ++seat) {
        wire.budget = 1;
        host.flush(wire);
        REQUIRE(wire.sent.size() == 1);
        const auto packet = wire.sent.back();
        wire.sent.clear();
        CHECK(packet.delivery == PacketTransport::Delivery::Reliable);
        REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
        CHECK(guest.receive(99, packet.bytes) == Admission::Accepted); // exact replay
        if (seat < 2) {
            CHECK_FALSE(guest.travelParty());
            CHECK_FALSE(room.loaded(2));
        }
        auto changed = *PartyCheckpointPacket::decode(packet.bytes);
        ++changed.profile.gold;
        CHECK(guest.receive(99, *PartyCheckpointPacket::encode(changed)) == Admission::Invalid);
    }
    REQUIRE(guest.travelParty());
    CHECK((*guest.travelParty())[1]->gold == 7654);
    CHECK((*guest.travelParty())[1]->progress.inventory.keys == 7);
    REQUIRE(room.loaded(2));
    wire.budget = 100;
    room.pump();
    CHECK(host.phase() == Phase::Running);
    CHECK(guest.phase() == Phase::Running);
    room.ready(MatchTransition::Travel, 9);
    CHECK(guest.receive(99, *encoded) == Admission::Stale);
    REQUIRE(guest.travelParty());
}

TEST_CASE("travel preparation is bounded under congestion and missing profiles time out",
          "[netplay][match-session][party-checkpoint]") {
    Room room;
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    const auto party = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.queuedControls() == 15); // 3 peers * (Prepare + 4 profiles)
    room.wires[1].result = PacketTransport::SendResult::Congested;
    host.flush(room.wires[1]);
    CHECK(host.queuedControls() == 15);
    CHECK_FALSE(host.prepare(9, MatchTransition::Travel, &party));
    CHECK(host.queuedControls() == 15);
    const auto prepare = message(host, MatchControlKind::Prepare);
    auto& guest = room.sessions[2];
    REQUIRE(guest.receive(99, prepare) == Admission::Accepted);
    guest.update(
        MatchSession::kLoadTimeout); // still playing to the stage's end: the host decides first
    CHECK(guest.phase() == Phase::Running);
    guest.update(MatchSession::kGuestLoadTimeout - MatchSession::kLoadTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::LoadTimeout);
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(room.loaded(2));
}

TEST_CASE("simulation waits for every machine to load including two local seats",
          "[netplay][match-session][multiplayer]") {
    MatchOwners owners{1, 2, 3, 4};
    SECTION("four machines") {}
    SECTION("two machines two seats each") {
        owners = {1, 1, 4, 4};
    }
    Room room(owners);
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    REQUIRE(room.loaded(1));
    room.pump();
    for (u8 peer = 2; peer < 4; ++peer) {
        if (room.present(peer)) {
            REQUIRE(room.loaded(peer));
            CHECK_FALSE(room.loaded(peer));
        }
    }
    room.pump();
    CHECK_FALSE(room.inputs[1].advance());
    CHECK_FALSE(room.sample(1, {}));
    CHECK(room.inputs[1].tick() == 1);
    auto& slow = room.sessions[4];
    CHECK(slow.receive(99, message(host, MatchControlKind::Commit)) == Admission::WrongPhase);
    REQUIRE(room.loaded(4));
    room.pump();
    REQUIRE(host.phase() == Phase::Running);
    CHECK_FALSE(room.inputs[1].advance()); // No real frame has arrived from any seat yet.
    room.completeOne();
    CHECK(room.inputs[1].tick() == 2);
    CHECK(room.inputs[4].tick() == 2);
    CHECK_FALSE(slow.prepare(8));
}

TEST_CASE("local devices map to frozen global seats without UI pointers",
          "[netplay][match-session][controls][multiplayer]") {
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    SessionInputs::Frame local;
    const Input devices;
    local[0].move = {{1, 0}, 1};
    local[0].throwPotion = true;
    local[0].menu.devices = &devices;
    local[0].menu.typed = "PRIVATE";
    local[0].menu.pointer = Vec2{1};
    local[1].move = {{-1, 0}, 1};
    local[1].aimDirection = Vec3{0.6f, 0, 0.8f};
    REQUIRE(room.sample(2, local));
    REQUIRE(room.sample(1, local));
    CHECK_FALSE(room.sample(1, local));
    room.pump();
    auto frame = room.advance(1);
    REQUIRE(frame);
    CHECK((*frame)[0].throwPotion);
    CHECK((*frame)[2].move.direction == Vec2{-1, 0});
    CHECK((*frame)[2].aimDirection == local[1].aimDirection);
    CHECK((*frame)[1].throwPotion);
    CHECK((*frame)[3].move.direction == Vec2{-1, 0});
    CHECK((*frame)[3].aimDirection == local[1].aimDirection);
    for (const auto& input : *frame) {
        CHECK(input.menu.devices == nullptr);
        CHECK(input.menu.typed.empty());
        CHECK_FALSE(input.menu.pointer);
    }
    const auto remote = room.advance(2);
    REQUIRE(remote);
    for (usize seat = 0; seat < frame->size(); ++seat) {
        CHECK((*remote)[seat].move.direction == (*frame)[seat].move.direction);
        CHECK((*remote)[seat].throwPotion == (*frame)[seat].throwPotion);
    }
    REQUIRE(room.inputs[1].complete());
    REQUIRE(room.inputs[2].complete());
    CHECK_FALSE(room.advance(1)); // No stale held input is invented.
}

TEST_CASE("spoofed trailing input cannot partially apply an otherwise valid command",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    InputCommand input;
    input.epoch = host.context().epoch;
    input.tick = room.inputs[1].tick();
    input.grant = host.context().grants[1];
    input.seat = 1;
    input.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    auto spoof = input;
    spoof.seat = 0;
    const std::array commands{input, spoof};
    RollbackExchange::Message message{input.epoch, 2, 1, {}, {commands.begin(), commands.end()}};
    const auto packet = RollbackExchange::encode(message);
    REQUIRE(packet);
    CHECK(host.receive(1, *packet) == Admission::Invalid);
    CHECK_FALSE(room.inputs[1].advance());
    REQUIRE(room.sample(1, {}));
    input.pressedButtons = 0;
    message.commands = {input};
    REQUIRE(host.receive(1, *RollbackExchange::encode(message)) == Admission::Accepted);
    const auto frame = room.inputs[1].advance();
    REQUIRE(frame);
    CHECK_FALSE(frame->commands[1].pressed(CommandPress::UsePotion));
    CHECK_FALSE(frame->commands[0].pressed(CommandPress::UsePotion));
    input.tick = room.inputs[1].tick() + RollbackInputs::kMaxAhead + 1;
    message.commands = {input};
    const auto future = RollbackExchange::encode(message);
    REQUIRE(future);
    CHECK(host.receive(1, *future) == Admission::Invalid);
}

TEST_CASE("ready input sampling leaves the caller's attack levels unchanged",
          "[netplay][match-session][input-pacing][queued-attack]") {
    const bool held = GENERATE(false, true);
    const bool pressed = GENERATE(false, true);
    Room room({1, 2, 0, 0});
    room.ready();
    auto& guest = room.sessions[2];
    SessionInputs::Frame input{};
    input[0].attack = held;
    input[0].attackPressed = pressed;
    REQUIRE(room.inputs[2].sampleReady());
    REQUIRE(room.sample(2, input));
    REQUIRE(room.inputs[2].inputs().redundantInputs().size() == 1);
    CHECK(room.inputs[2].inputs().redundantInputs().front().held(CommandHeld::Attack) == held);
    CHECK(room.inputs[2].inputs().redundantInputs().front().pressed(CommandPress::Attack) ==
          pressed);
}

TEST_CASE("a guest still loading a later stage at the deadline is dropped and the rest go on",
          "[netplay][match-session][drop]") {
    Room room({1, 2, 3, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    const auto profiles = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
    room.pump();
    REQUIRE(room.loaded(1));
    REQUIRE(room.loaded(2));
    room.pump(); // peer three never loads: minimised, or hung
    REQUIRE(host.phase() == Phase::Loading);
    host.update(MatchSession::kLoadTimeout);
    CHECK(host.phase() == Phase::Running);
    CHECK(host.dropped(3));
    room.pump();
    CHECK(room.sessions[2].phase() == Phase::Running);
    CHECK(room.sessions[2].dropped(3));
    CHECK(room.sessions[3].phase() == Phase::Stopped);
    CHECK(room.sessions[3].stopReason() == MatchStop::Left);
}

TEST_CASE("play begun by a guest's last word counts everyone as heard from then",
          "[netplay][match-session][drop]") {
    // The host has loaded and waits; the last guest's Ready comes long after, as when the
    // host's own load ran past the others'. Nobody is silent the moment play begins.
    Room room({1, 2, 0, 0});
    const MatchSession::Time loadedAt{};
    REQUIRE(room.sessions[1].prepare(7));
    room.pump(loadedAt);
    REQUIRE(room.sessions[2].phase() == Phase::Loading);
    room.sessions[1].update(0, loadedAt);
    REQUIRE(room.loaded(1));
    REQUIRE(room.loaded(2));
    const auto readyAt = loadedAt + std::chrono::seconds{16};
    // One poll brings the guest's heartbeat and then the Ready that completes the barrier.
    room.sessions[2].flush(room.wires[2], readyAt);
    auto sent = std::move(room.wires[2].sent);
    room.wires[2].sent.clear();
    std::ranges::stable_partition(
        sent, [](const auto& packet) { return RollbackExchange::recognizes(packet.bytes); });
    REQUIRE(RollbackExchange::recognizes(sent.front().bytes));
    for (const auto& packet : sent) {
        const auto admitted = room.sessions[1].receive(1, packet.bytes, readyAt);
        REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
    }
    REQUIRE(room.sessions[1].phase() == Phase::Running);
    CHECK(room.sessions[1].quietPeers(MatchSession::kQuietAfter, readyAt).empty());
    CHECK(room.sessions[1].quietSeconds(readyAt) < MatchSession::kQuietAfter);
}

TEST_CASE("load timeout aborts even before the prepare message reaches a guest",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    host.update(-1);
    host.update(std::numeric_limits<f64>::quiet_NaN());
    host.update(std::numeric_limits<f64>::infinity());
    host.update(MatchSession::kLoadTimeout - 0.5);
    CHECK(host.phase() == Phase::Loading);
    host.update(0.5);
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::LoadTimeout);
    room.pump();
    CHECK(room.sessions[2].phase() == Phase::Stopped);
    CHECK(room.sessions[2].stopReason() == MatchStop::LoadTimeout);
}

TEST_CASE("a guest's lost link drops it while the host and the rest carry on",
          "[netplay][match-session][drop]") {
    Room room;
    room.ready();
    auto& host = room.sessions[1];
    auto guestStop = MatchStop::Disconnected;
    SECTION("running") {}
    SECTION("loading next stage") {
        room.completeOne();
        const auto profiles = room.party();
        REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
        room.pump();
        guestStop = MatchStop::TransportFailure; // its retained source certificate fails
    }
    REQUIRE(host.disconnected(1)); // remote peer 2, NOT host peer 1
    CHECK(host.dropped(2));
    CHECK(host.phase() != Phase::Stopped);
    CHECK_FALSE(room.sessions[2].disconnected(99)); // its host is gone
    room.pump();
    CHECK(room.sessions[2].phase() == Phase::Stopped);
    CHECK(room.sessions[2].stopReason() == guestStop);
    for (const u8 peer : {u8{1}, u8{3}, u8{4}}) {
        CAPTURE(peer);
        CHECK(room.sessions[peer].phase() != Phase::Stopped);
        CHECK(room.sessions[peer].dropped(2));
        CHECK_FALSE(room.sessions[peer].dropped(3));
        const auto drops = room.sessions[peer].takeDrops();
        REQUIRE(drops.size() == 1);
        CHECK(drops.front().peer == 2);
        CHECK(room.sessions[peer].takeDrops().empty());
        REQUIRE(room.sessions[peer].dropsFor(drops.front().epoch).size() == 1);
        CHECK(room.sessions[peer].dropsFor(drops.front().epoch).front().tick == drops.front().tick);
        CHECK(room.sessions[peer].dropsFor(drops.front().epoch + 1).front().tick == 1);
        CHECK((room.inputs[peer].inputs().suspend(2, drops.front().tick) ||
               room.sessions[peer].phase() == Phase::Loading));
    }
    CHECK_FALSE(host.drop(2)); // once
    CHECK_FALSE(host.drop(1)); // never the host
}

TEST_CASE("send detects a closed guest before its disconnect event without stopping the match",
          "[netplay][match-session][drop][send-disconnect]") {
    const auto result =
        GENERATE(PacketTransport::SendResult::Disconnected, PacketTransport::SendResult::Invalid);
    Room room({1, 2, 3, 0});
    room.ready();
    REQUIRE(room.sample(1));
    auto& host = room.sessions[1];
    auto& wire = room.wires[1];
    REQUIRE(host.queuedControls() == 0);
    wire.failingConnection = 1; // guest 2; guest 3's connection is healthy
    wire.result = result;
    host.flush(wire);
    if (result == PacketTransport::SendResult::Invalid) {
        CHECK(host.phase() == Phase::Stopped);
        CHECK(host.stopReason() == MatchStop::TransportFailure);
        CHECK_FALSE(host.dropped(2));
        return;
    }
    REQUIRE(host.phase() == Phase::Running);
    REQUIRE(host.dropped(2));
    CHECK_FALSE(host.dropped(3));
    CHECK(host.disconnected(1)); // the delayed provider event is harmless
    host.flush(wire);
    REQUIRE_FALSE(wire.sent.empty());
    for (const auto& packet : wire.sent) {
        REQUIRE(packet.connection == 2);
        CHECK(room.sessions[3].receive(99, packet.bytes) == Admission::Accepted);
    }
    CHECK(host.phase() == Phase::Running);
    CHECK(room.sessions[3].phase() == Phase::Running);
    CHECK(room.sessions[3].dropped(2));
}

TEST_CASE("a send-time guest departure can retire the draining source exchange safely",
          "[netplay][match-session][drop][send-disconnect]") {
    Room room({1, 2, 0, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    const auto profiles = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
    room.pump();
    REQUIRE(room.loaded(1));
    REQUIRE(host.phase() == Phase::Loading);
    REQUIRE(host.queuedControls() == 0);
    room.wires[1].result = PacketTransport::SendResult::Disconnected;
    host.flush(room.wires[1]);
    CHECK(host.dropped(2));
    CHECK(host.phase() == Phase::Running);
    CHECK(host.disconnected(1));
    host.flush(room.wires[1]);
    CHECK(host.phase() == Phase::Running);
}

TEST_CASE("a guest detecting its host closure in send still stops the match",
          "[netplay][match-session][drop][send-disconnect]") {
    Room room({1, 2, 0, 0});
    room.ready();
    REQUIRE(room.sample(2));
    room.wires[2].result = PacketTransport::SendResult::Disconnected;
    room.sessions[2].flush(room.wires[2]);
    CHECK(room.sessions[2].phase() == Phase::Stopped);
    CHECK_FALSE(room.sessions[2].dropped(1));
}

TEST_CASE("a dropped guest's shop messages never hold up the rest of the shop",
          "[netplay][match-session][drop][online-shop]") {
    Room room({1, 2, 3, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    const auto party = room.party();
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    setup.maxima = {1000, 100, 1000};
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    // The setup waits for each guest when guest two's link closes.
    REQUIRE(host.shop().outgoing().size() == 2);
    REQUIRE(host.disconnected(1));
    REQUIRE(host.dropped(2));
    // As the provider does, a connection it does not hold refuses every send.
    struct Provider final : PacketTransport {
        usize shopPackets = 0;
        usize refused = 0;
        SendResult send(Connection connection, std::span<const u8> bytes,
                        Delivery /*delivery*/) override {
            if (connection != 2) {
                ++refused;
                return SendResult::Disconnected;
            }
            if (ShopSync::recognizes(bytes)) {
                ++shopPackets;
            }
            return SendResult::Sent;
        }
        std::vector<Event> poll() override { return {}; }
        std::optional<Statistics> statistics(Connection /*connection*/) const override {
            return std::nullopt;
        }
        void close(Connection /*connection*/) override {}
    } provider;
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        host.flush(provider);
    }
    CHECK(provider.refused == 0);
    CHECK(host.shop().outgoing().empty());
    CHECK(provider.shopPackets == 1); // guest three's setup
    // Every later step still names the dropped guest; its copy is let go each time.
    const auto step = host.shop().advance();
    REQUIRE(step);
    REQUIRE(host.shop().publish(*step));
    host.flush(provider);
    CHECK(provider.refused == 0);
    CHECK(host.shop().outgoing().empty());
    CHECK(provider.shopPackets == 2);
    CHECK(host.phase() == Phase::Loading);
}

TEST_CASE("a dropped guest holds none of the shops the host opens after its drop",
          "[netplay][match-session][drop][online-shop]") {
    const bool earlier = GENERATE(false, true);
    CAPTURE(earlier);
    Room room({1, 2, 0, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    const auto party = room.party();
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    setup.maxima = {1000, 100, 1000};
    // Dropped in the level before, or while the shop loads.
    if (earlier) {
        REQUIRE(host.disconnected(1));
    }
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    if (!earlier) {
        REQUIRE(host.disconnected(1));
    }
    REQUIRE(host.dropped(2));
    CHECK_FALSE(host.shop().finished());
    REQUIRE(host.shop().finish());
    CHECK(host.shop().finished()); // the host's own lane was the last one owed
}

TEST_CASE("a guest dropped mid-shop leaves its lane at the host's next step on every peer",
          "[netplay][match-session][drop][online-shop]") {
    Room room({1, 2, 3, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto party = room.party();
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    setup.maxima = {1000, 100, 1000};
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    const auto epoch = host.context().epoch;
    for (u32 step = 0; step < 3; ++step) {
        const auto ran = host.shop().advance();
        REQUIRE(ran);
        REQUIRE(host.shop().publish(*ran));
    }
    REQUIRE(host.disconnected(2)); // guest three's link closes after the third step
    REQUIRE(host.dropped(3));
    const auto drops = host.dropsFor(epoch);
    REQUIRE(drops.size() == 1);
    CHECK(drops.front().tick == 4);
    CHECK_FALSE(host.shopLaneGone(3, epoch, 2));
    CHECK(host.shopLaneGone(3, epoch, 3));
    CHECK_FALSE(host.shopLaneGone(2, epoch, 3));
    // Controls go out ahead of shop steps, so the drop reaches guest two before the steps the
    // host ran with the lane still open; guest two still ends it only from the fourth.
    host.flush(room.wires[1]);
    const auto sent = std::move(room.wires[1].sent);
    room.wires[1].sent.clear();
    for (const auto& packet : sent) {
        if (packet.connection == 1) {
            const auto admitted = guest.receive(99, packet.bytes);
            CHECK((admitted == Admission::Accepted || admitted == Admission::Stale));
        }
    }
    REQUIRE(guest.dropped(3));
    CHECK(guest.dropsFor(epoch) == drops);
    CHECK_FALSE(guest.shopLaneGone(3, epoch, 2));
    CHECK(guest.shopLaneGone(3, epoch, 3));
}

TEST_CASE("transport send failure terminates loading instead of pretending a start succeeded",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    room.wires[1].result = PacketTransport::SendResult::Disconnected;
    host.flush(room.wires[1]);
    CHECK(host.phase() == Phase::Stopped);
    CHECK_FALSE(room.inputs[1].advance());
    CHECK_FALSE(room.loaded(1));
    CHECK(host.queuedControls() == 0);
}

TEST_CASE("match lifecycle drives real four player movement events and one shared camera",
          "[netplay][match-session][multiplayer][party-motion]") {
    Room room({1, 2, 2, 1});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    WorldCollision collision;
    CollisionTriangle first;
    first.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, -200}, Vec3{200, 0, 200}};
    first.normal = {0, 1, 0};
    first.object = 0;
    auto second = first;
    second.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{-200, 0, 200}};
    collision.build({first, second});
    std::array<PlayerRuntime, 4> players;
    std::array<Vec3, 4> starts{};
    std::array<CameraSubject, 4> subjects;
    for (usize seat = 0; seat < players.size(); ++seat) {
        starts[seat] = {static_cast<f32>(seat) * 2 - 3, 0, 0};
        players[seat].actor.spawn(static_cast<s32>(seat), {}, nullptr, starts[seat], 0);
        subjects[seat] = {players[seat].actor.position(), players[seat].actor.followPoint()};
    }
    TowerCamera camera;
    const CameraRange range;
    CameraView view;
    camera.reset(subjects, {}, range, view);
    std::array<u32, 4> potions{};
    std::array<u32, 4> toggles{};
    PartyMotion::Events events;
    events.perform = [&](usize player, PartyMotion::Action action) {
        REQUIRE(action == PartyMotion::Action::NoPotion);
        ++potions[player];
    };
    events.select = [&](usize player, const SelectorInput& input, s32 /*ticks*/) {
        if (input.up) {
            ++toggles[player];
        }
    };
    events.limitMovement = [&](usize player, const Vec3& from, const Vec3& to) {
        const auto& actor = players[player].actor;
        return CameraMovementLimit::constrain(from, to, camera.attention(), camera.camera(), view,
                                              actor.followPoint() - actor.position());
    };
    for (u32 tick = 0; tick < 180; ++tick) {
        SessionInputs::Frame local;
        for (auto& input : local) {
            input.move = {{1, 0}, 1};
            input.usePotion = tick == 0 || tick == 90;
            input.selector.up = input.usePotion;
        }
        REQUIRE(room.sample(1, local));
        REQUIRE(room.sample(2, local));
        room.pump();
        const auto inputs = room.advance(1);
        REQUIRE(inputs);
        const auto guestInputs = room.advance(2);
        REQUIRE(guestInputs);
        for (usize seat = 0; seat < inputs->size(); ++seat) {
            CHECK((*guestInputs)[seat].move.direction == (*inputs)[seat].move.direction);
            CHECK((*guestInputs)[seat].move.magnitude == (*inputs)[seat].move.magnitude);
            CHECK((*guestInputs)[seat].usePotion == (*inputs)[seat].usePotion);
            CHECK((*guestInputs)[seat].selector.up == (*inputs)[seat].selector.up);
        }
        const auto moved = PartyMotion::step(players, *inputs, false, camera.yaw(), 1, 1.0f / 60,
                                             collision, events);
        camera.update(moved, {}, range, view, 1.0f / 60);
        REQUIRE(room.inputs[1].complete());
        REQUIRE(room.inputs[2].complete());
    }
    for (usize seat = 0; seat < players.size(); ++seat) {
        CHECK(potions[seat] == 2);
        CHECK(toggles[seat] == 2);
        CHECK(glm::length(players[seat].actor.position() - starts[seat]) > 1);
    }
}
} // namespace

TEST_CASE("a guest's direct links feed the input exchange and nothing else",
          "[netplay][match-session][direct-link]") {
    const MatchOwners owners{1, 2, 3, 0};
    const std::array star{MatchLink{1, 99}};
    const std::array hostStar{MatchLink{2, 1}, MatchLink{3, 2}};
    // The host has none; the host is never a direct peer; owners only, one link each,
    // on a connection of its own.
    CHECK_FALSE(MatchSession{}.open(1, owners, hostStar, 1, std::array{MatchLink{3, 5}}));
    CHECK_FALSE(MatchSession{}.open(2, owners, star, 1, std::array{MatchLink{1, 5}}));
    CHECK_FALSE(MatchSession{}.open(2, owners, star, 1, std::array{MatchLink{4, 5}}));
    CHECK_FALSE(MatchSession{}.open(2, owners, star, 1, std::array{MatchLink{3, 99}}));
    CHECK_FALSE(
        MatchSession{}.open(2, owners, star, 1, std::array{MatchLink{3, 5}, MatchLink{3, 6}}));
    MatchSession guest;
    REQUIRE(guest.open(2, owners, star, 1, std::array{MatchLink{3, 5}}));
    CHECK(guest.directLinks() == 1);
    // Only input packets are taken from a direct link; the host still says everything else.
    const std::vector<u8> stray{1, 2, 3, 4};
    CHECK(guest.receive(5, stray) == MatchSession::Admission::WrongPeer);
    const auto inputs = RollbackExchange::encode({1, 3, 1, {}, {}, 1});
    REQUIRE(inputs);
    CHECK(guest.receive(5, *inputs) == MatchSession::Admission::Stale); // routed; no stream yet
    CHECK(guest.receive(6, *inputs) == MatchSession::Admission::WrongPeer);
    guest.directLost(5);
    CHECK(guest.directLinks() == 0);
    CHECK(guest.receive(5, *inputs) == MatchSession::Admission::WrongPeer);
    CHECK(guest.phase() == MatchSession::Phase::Lobby);
}

TEST_CASE("a drop made once the host prepares reaches a guest still finishing the stage",
          "[netplay][match-session][online-shop][drop]") {
    Room room({1, 2, 3, 0});
    room.ready();
    room.completeOne();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[3];
    const auto party = room.party();
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    REQUIRE(host.disconnected(1)); // guest two leaves as the tally begins
    host.flush(room.wires[1]);
    const auto sent = std::move(room.wires[1].sent);
    room.wires[1].sent.clear();
    // Prepare, the profiles, the drop and the shop setup arrive in one batch, before guest
    // three has finished the stage it is leaving.
    for (const auto& packet : sent) {
        if (packet.connection == 2) {
            const auto admitted = guest.receive(99, packet.bytes);
            CHECK((admitted == Admission::Accepted || admitted == Admission::Stale));
        }
    }
    REQUIRE(guest.pendingNativeContext());
    CHECK(guest.dropped(2));
    CHECK(guest.dropsFor(guest.context().epoch).empty()); // not in the stage it finishes
    REQUIRE(guest.completeNativeBoundary(guest.context().epoch, room.completed[3]));
    REQUIRE(guest.context() == host.context());
    const auto drops = guest.dropsFor(guest.context().epoch);
    REQUIRE(drops.size() == 1);
    CHECK(drops.front().peer == 2);
    CHECK(drops.front().tick == host.dropsFor(host.context().epoch).front().tick);
}
