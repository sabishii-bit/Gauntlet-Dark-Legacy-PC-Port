#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeMechanic.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/netplay/HostedRoom.h"
#include "game/netplay/NativePlaySetup.h"
#include "game/netplay/OnlineSession.h"
#include "game/rollback/InputCommand.h"
#include "game/rollback/RollbackSessionHarness.h"

namespace {
using namespace gdl;
using namespace gdl::game;
/** A build offering three mechanics as a room's rules. */
const MechanicCatalog& offered() {
    static const MechanicCatalog kCatalog = [] {
        MechanicCatalog mechanics;
        for (const std::string_view id : {"alpha", "beta", "gamma"}) {
            REQUIRE(mechanics.add(test::FakeMechanic::descriptor(id)));
        }
        return mechanics;
    }();
    return kCatalog;
}
struct Wire final : PacketTransport {
    Wire* remote = nullptr;
    std::vector<Event> incoming;
    bool congested = false;
    SendResult send(Connection id, std::span<const u8> bytes, Delivery /*delivery*/) override {
        if (congested) {
            return SendResult::Congested;
        }
        remote->incoming.push_back({EventType::Message, id, {bytes.begin(), bytes.end()}, {}});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return std::exchange(incoming, {}); }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection id) override {
        if (remote) {
            remote->incoming.push_back({EventType::Disconnected, id, {}, {}});
        }
    }
};
struct Pair {
    Wire a, b;
    HostedRoom hostRoom, guestRoom;
    OnlineSession host, guest;
    test::RollbackSessionHarness hostInputs, guestInputs;
    Pair(u8 hostPlayers = 1, u8 guestPlayers = 1, const std::string& version = "test",
         const std::string& content = std::string(64, 'a'))
        : hostRoom(a, true, hostPlayers, "test", std::string(64, 'a')),
          guestRoom(b, false, guestPlayers, version, content),
          host(hostRoom, hostRoom.transport(), true, hostPlayers),
          guest(guestRoom, guestRoom.transport(), false, guestPlayers) {
        a.remote = &b;
        b.remote = &a;
        a.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        b.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        const OnlineSession::NativeIdentity identity{"test", "test-runtime", std::string(64, 'a'),
                                                     std::string(64, 'b'), "en"};
        OnlineSession::NativeRules rules;
        rules.scene = 1;
        REQUIRE(host.configureNative(identity, rules));
        REQUIRE(guest.configureNative(
            {version, identity.runtime, content, identity.data, identity.language}));
        CharacterProfile profile;
        profile.name = "TEST";
        REQUIRE(host.select(std::vector<CharacterProfile>(hostPlayers, profile)));
        REQUIRE(guest.select(std::vector<CharacterProfile>(guestPlayers, profile)));
    }
    ~Pair() {
        hostInputs.clear();
        guestInputs.clear();
        host.leave();
        guest.leave();
        hostRoom.leave();
        guestRoom.leave();
        a.remote = nullptr;
        b.remote = nullptr;
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&&) = delete;
    Pair& operator=(Pair&&) = delete;
    void pump() {
        for (s32 i = 0; i < 10; ++i) {
            host.update(0);
            guest.update(0);
            for (auto* session : {&host, &guest}) {
                if (!session->nativeSetupPending()) {
                    continue;
                }
                const auto& agreed = *session->nativeContract();
                const NativePlaySetup::Contract contract{agreed.rules.scene,
                                                         agreed.rules.autoMelee,
                                                         agreed.rules.autoActivateItems,
                                                         agreed.rules.expandedShop,
                                                         agreed.rules.autoAim,
                                                         agreed.identity.build,
                                                         agreed.identity.content,
                                                         agreed.identity.data,
                                                         &offered()};
                const auto setup = NativePlaySetup::create(GameConfig{}, session->room()->settings,
                                                           *session->party(), {}, contract);
                REQUIRE(setup);
                REQUIRE(session->confirmNativeSetup(setup->digest()));
            }
        }
    }
};
} // namespace
TEST_CASE("peers missing replay capabilities cannot join even with the same build name",
          "[netplay][hosted-room]") {
    for (const u8 oldProtocol :
         {u8{1},  u8{13}, u8{14}, u8{15}, u8{16}, u8{17}, u8{18}, u8{19}, u8{20}, u8{21},
          u8{22}, u8{23}, u8{24}, u8{25}, u8{26}, u8{27}, u8{28}, u8{29}, u8{30}, u8{31}}) {
        Pair pair;
        pair.guest.update(0);
        bool rewrote = false;
        for (auto& event : pair.a.incoming) {
            if (event.type == PacketTransport::EventType::Message && event.bytes.size() >= 2) {
                event.bytes[1] = oldProtocol;
                rewrote = true;
            }
        }
        REQUIRE(rewrote);
        pair.pump();
        CHECK_FALSE(pair.guest.connected());
        REQUIRE(pair.host.room());
        CHECK(pair.host.room()->members.size() == 1);
    }
}

TEST_CASE("hosted exact-input packets fit a single provider fragment", "[netplay][hosted-room]") {
    const bool console = GENERATE(false, true);
    Pair pair(2, 1);
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    REQUIRE(pair.host.start());
    pair.pump();
    REQUIRE(pair.host.phase() == OnlineSession::Phase::Active);
    REQUIRE(pair.b.incoming.empty());

    std::vector<InputCommand> inputs(InputPacket::kMaxCommands);
    for (usize index = 0; index < inputs.size(); ++index) {
        inputs[index].epoch = 1;
        inputs[index].tick = index / 2 + 1;
        inputs[index].seat = static_cast<u8>(index % 2);
        inputs[index].grant = 1;
        if (console) {
            inputs[index].console = ConsoleAction{2, "spawn " + std::string(154, 'a')};
        }
    }
    const RollbackExchange::Message message{1, 1, 1, {}, inputs, 1};
    CHECK_FALSE(RollbackExchange::encode(message)); // the full burst needs multiple packets
    RollbackExchange exchange;
    const std::array links{RollbackExchange::Link{2, 7}};
    REQUIRE(exchange.open(1, 1, {1, 1, 2, 0}, {1, 1, 1, 0}, 0, links));
    REQUIRE(exchange.publish(inputs, 1));
    REQUIRE(exchange.flush(pair.hostRoom.transport()));
    REQUIRE(pair.b.incoming.size() > 1);
    usize delivered = 0;
    for (const auto& event : pair.b.incoming) {
        const auto& wrapped = event.bytes;
        REQUIRE(wrapped.size() <= 1000);
        const auto packet = RollbackExchange::decode(std::span{wrapped}.subspan(1));
        REQUIRE(packet);
        REQUIRE_FALSE(packet->commands.empty());
        std::map<u64, usize> seatsPerTick;
        for (const auto& command : packet->commands) {
            REQUIRE(delivered < inputs.size());
            CHECK(InputPacket::encode(std::span{&command, 1}) ==
                  InputPacket::encode(std::span{&inputs[delivered], 1}));
            ++delivered;
            ++seatsPerTick[command.tick];
        }
        for (const auto& [tick, seats] : seatsPerTick) {
            CAPTURE(tick);
            CHECK(seats == 2); // a datagram never splits an owned-seat frame
        }
    }
    CHECK(delivered == inputs.size());
}

TEST_CASE("self hosted admission starts existing online gameplay without a room server",
          "[netplay][hosted-room]") {
    for (u8 players = 1; players <= 3; ++players) {
        Pair pair(players, 1);
        pair.a.congested = true;
        pair.pump();
        CHECK(pair.guest.phase() == OnlineSession::Phase::Joining);
        pair.a.congested = false;
        pair.pump();
        REQUIRE(pair.host.connected());
        REQUIRE(pair.guest.connected());
        REQUIRE(pair.guest.seats().size() == 1);
        CHECK(pair.guest.seats()[0] == players);
        REQUIRE(pair.host.ready());
        REQUIRE(pair.guest.ready());
        pair.pump();
        REQUIRE(pair.host.start());
        pair.pump();
        REQUIRE(pair.host.phase() == OnlineSession::Phase::Active);
        REQUIRE(pair.guest.phase() == OnlineSession::Phase::Active);
        REQUIRE_FALSE(pair.host.confirmedNativeSetup().empty());
        CHECK(pair.host.confirmedNativeSetup() == pair.guest.confirmedNativeSetup());
        REQUIRE(pair.host.match().prepare(1));
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Loading);
        CHECK_FALSE(pair.host.match().loaded());
        CHECK_FALSE(pair.guest.match().loaded());
        REQUIRE(pair.hostInputs.bind(pair.host.match()));
        REQUIRE(pair.guestInputs.bind(pair.guest.match()));
        REQUIRE(pair.host.match().loaded());
        REQUIRE(pair.guest.match().loaded());
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Running);
        // The agreed one-tick local buffer is executable for every owned seat;
        // admission alone is not a substitute for the native input consumer.
        REQUIRE(pair.hostInputs.advance());
        REQUIRE(pair.guestInputs.advance());
        REQUIRE(pair.hostInputs.complete());
        REQUIRE(pair.guestInputs.complete());
        CHECK(pair.host.match().nativeConfirmed() == 1);
        CHECK(pair.guest.match().nativeConfirmed() == 1);
        // A guest leaving mid-match is dropped; the host plays on alone.
        pair.guest.leave();
        pair.pump();
        CHECK(pair.host.phase() == OnlineSession::Phase::Active);
        CHECK(pair.host.match().phase() == MatchSession::Phase::Running);
        CHECK(pair.host.match().dropped(2));
    }
}
TEST_CASE("hosted room admission rejects mismatched builds assets and full parties",
          "[netplay][hosted-room]") {
    SECTION("build") {
        Pair pair(1, 1, "different");
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Version);
        CHECK(pair.guest.failureDetail() == "Game versions do not match");
        CHECK(pair.host.phase() == OnlineSession::Phase::Lobby);
    }
    SECTION("assets") {
        Pair pair(1, 1, "test", std::string(64, 'b'));
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Assets);
        CHECK(pair.guest.failureDetail() == "Game assets do not match");
    }
    SECTION("full") {
        Pair pair(3, 2);
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
        CHECK(pair.guest.failureDetail() == "Room has no free player slots");
    }
}
TEST_CASE("a hosted lobby survives guest departure and resets readiness",
          "[netplay][hosted-room]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    pair.guest.leave();
    pair.pump();
    REQUIRE(pair.host.phase() == OnlineSession::Phase::Lobby);
    REQUIRE(pair.host.room()->members.size() == 1);
    CHECK_FALSE(pair.host.room()->members[0].ready);
}
TEST_CASE("room rules are host owned replicated and invalidate every ready vote",
          "[netplay][hosted-room][room-settings]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    const auto revision = pair.host.room()->revision;
    const RoomSettings changed{3, 2, 1, 1, 0b101}; // solo cameras and the rule bits travel too
    CHECK_FALSE(pair.guestRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.settings({3, 2, 1, 2}));    // no third camera rule
    CHECK_FALSE(pair.hostRoom.settings({1, 2, 1}));       // already two joined
    CHECK_FALSE(pair.hostRoom.settings({4, 3, 0}));
    REQUIRE(pair.hostRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.start(revision));
    pair.pump();
    CHECK(pair.host.room()->settings == changed);
    CHECK(pair.guest.room()->settings == changed);
    CHECK(pair.host.room()->revision > revision);
    CHECK_FALSE(pair.host.room()->members[0].ready);
    CHECK_FALSE(pair.host.room()->members[1].ready);
    CHECK_FALSE(pair.host.start());
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    REQUIRE(pair.host.start());
    pair.pump();
    CHECK_FALSE(pair.hostRoom.settings({4, 0, 0}));
    CHECK(pair.guest.room()->settings == changed);
}
TEST_CASE("room capacity is enforced when a guest requests multiple local seats",
          "[netplay][hosted-room][room-settings]") {
    Pair pair(1, 2);
    REQUIRE(pair.hostRoom.settings({2, 1, 0}));
    pair.pump();
    CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
    REQUIRE(pair.host.room());
    CHECK(pair.host.room()->members.size() == 1);
}

namespace {
/** Wires that reach each other by address, as the real transport's cards do. */
struct MeshWire final : PacketTransport {
    struct Peer {
        MeshWire* wire;
        Connection id;
    };
    std::string name;
    std::vector<MeshWire*>* mesh = nullptr;
    std::vector<Event> incoming;
    std::map<Connection, Peer> links;
    std::map<Connection, usize> sentOn;
    Connection next = 1;
    std::string address() const override { return "card:" + name; }
    std::optional<Connection> dial(const std::string& card) override {
        for (auto* wire : *mesh) {
            if (wire != this && wire->address() == card) {
                return connect(*wire);
            }
        }
        return std::nullopt;
    }
    Connection connect(MeshWire& other) {
        const auto mine = next++;
        const auto theirs = other.next++;
        links[mine] = {&other, theirs};
        other.links[theirs] = {this, mine};
        incoming.push_back({EventType::Connected, mine, {}, {}});
        other.incoming.push_back({EventType::Connected, theirs, {}, {}});
        return mine;
    }
    SendResult send(Connection id, std::span<const u8> bytes, Delivery /*delivery*/) override {
        const auto found = links.find(id);
        if (found == links.end()) {
            return SendResult::Disconnected;
        }
        ++sentOn[id];
        found->second.wire->incoming.push_back(
            {EventType::Message, found->second.id, {bytes.begin(), bytes.end()}, {}});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return std::exchange(incoming, {}); }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection id) override {
        const auto found = links.find(id);
        if (found == links.end()) {
            return;
        }
        const auto peer = found->second;
        links.erase(found);
        // Both ends hear that the connection ended, as the real transport reports.
        incoming.push_back({EventType::Disconnected, id, {}, {}});
        if (peer.wire->links.erase(peer.id) != 0) {
            peer.wire->incoming.push_back({EventType::Disconnected, peer.id, {}, {}});
        }
    }
    std::optional<Connection> linkTo(const MeshWire& other) const {
        for (const auto& [id, peer] : links) {
            if (peer.wire == &other) {
                return id;
            }
        }
        return std::nullopt;
    }
};
struct Trio {
    std::vector<MeshWire*> mesh;
    MeshWire a, b, c;
    HostedRoom hostRoom, firstRoom, secondRoom;
    OnlineSession host, first, second;
    test::RollbackSessionHarness hostInputs, firstInputs, secondInputs;
    Trio()
        : hostRoom(a, true, 1, "test", std::string(64, 'a')),
          firstRoom(b, false, 1, "test", std::string(64, 'a')),
          secondRoom(c, false, 1, "test", std::string(64, 'a')),
          host(hostRoom, hostRoom.transport(), true, 1),
          first(firstRoom, firstRoom.transport(), false, 1),
          second(secondRoom, secondRoom.transport(), false, 1) {
        a.name = "host";
        b.name = "first";
        c.name = "second";
        mesh = {&a, &b, &c};
        for (auto* wire : mesh) {
            wire->mesh = &mesh;
        }
        // Each guest reaches the host first, as the real transport does.
        REQUIRE(b.dial(a.address()));
        REQUIRE(c.dial(a.address()));
        const OnlineSession::NativeIdentity identity{"test", "test-runtime", std::string(64, 'a'),
                                                     std::string(64, 'b'), "en"};
        OnlineSession::NativeRules rules;
        rules.scene = 1;
        REQUIRE(host.configureNative(identity, rules));
        REQUIRE(first.configureNative(identity));
        REQUIRE(second.configureNative(identity));
        CharacterProfile profile;
        profile.name = "TEST";
        for (auto* session : {&host, &first, &second}) {
            REQUIRE(session->select(std::vector<CharacterProfile>(1, profile)));
        }
    }
    ~Trio() {
        hostInputs.clear();
        firstInputs.clear();
        secondInputs.clear();
        for (auto* session : {&host, &first, &second}) {
            session->leave();
        }
        for (auto* room : {&hostRoom, &firstRoom, &secondRoom}) {
            room->leave();
        }
    }
    Trio(const Trio&) = delete;
    Trio& operator=(const Trio&) = delete;
    Trio(Trio&&) = delete;
    Trio& operator=(Trio&&) = delete;
    static std::vector<InputCommand> localCommands(OnlineSession& session,
                                                   const test::RollbackSessionHarness& timeline) {
        std::vector<InputCommand> result;
        const auto& context = session.match().context();
        for (usize seat = 0; seat < context.owners.size(); ++seat) {
            if (context.owners[seat] != session.match().localPeer()) {
                continue;
            }
            InputCommand command;
            command.epoch = context.epoch;
            command.tick = timeline.sampleTick();
            command.grant = context.grants[seat];
            command.seat = static_cast<u8>(seat);
            result.push_back(command);
        }
        return result;
    }
    /** Everyone samples, the wires carry, everyone advances and confirms. */
    void step() {
        const std::array pairs{std::pair{&host, &hostInputs}, std::pair{&first, &firstInputs},
                               std::pair{&second, &secondInputs}};
        for (const auto& [session, inputs] : pairs) {
            if (inputs->sampleReady()) {
                REQUIRE(inputs->sample(localCommands(*session, *inputs)));
            }
        }
        pump();
        for (const auto& [session, inputs] : pairs) {
            if (inputs->advance()) {
                REQUIRE(inputs->complete());
            }
        }
    }
    void pump() {
        for (s32 i = 0; i < 12; ++i) {
            for (auto* session : {&host, &first, &second}) {
                session->update(0);
                if (!session->nativeSetupPending()) {
                    continue;
                }
                const auto& agreed = *session->nativeContract();
                const NativePlaySetup::Contract contract{agreed.rules.scene,
                                                         agreed.rules.autoMelee,
                                                         agreed.rules.autoActivateItems,
                                                         agreed.rules.expandedShop,
                                                         agreed.rules.autoAim,
                                                         agreed.identity.build,
                                                         agreed.identity.content,
                                                         agreed.identity.data,
                                                         &offered()};
                const auto setup = NativePlaySetup::create(GameConfig{}, session->room()->settings,
                                                           *session->party(), {}, contract);
                REQUIRE(setup);
                REQUIRE(session->confirmNativeSetup(setup->digest()));
            }
        }
    }
};
} // namespace

TEST_CASE("three hosted peers link the guests directly and the match uses the link",
          "[netplay][hosted-room][direct-link]") {
    Trio trio;
    trio.pump();
    REQUIRE(trio.host.room());
    REQUIRE(trio.host.room()->members.size() == 3);
    CHECK(trio.host.directLinks() == 0);
    CHECK(trio.first.directLinks() == 1);
    CHECK(trio.second.directLinks() == 1);
    const auto direct = trio.b.linkTo(trio.c);
    REQUIRE(direct);
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        REQUIRE(session->ready());
    }
    trio.pump();
    REQUIRE(trio.host.start());
    trio.pump();
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        REQUIRE(session->phase() == OnlineSession::Phase::Active);
    }
    CHECK(trio.host.match().directLinks() == 0);
    CHECK(trio.first.match().directLinks() == 1);
    CHECK(trio.second.match().directLinks() == 1);
    // A stage starts; the first guest's inputs reach the second on the direct link.
    REQUIRE(trio.host.match().prepare(1));
    trio.pump();
    REQUIRE(trio.hostInputs.bind(trio.host.match()));
    REQUIRE(trio.firstInputs.bind(trio.first.match()));
    REQUIRE(trio.secondInputs.bind(trio.second.match()));
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        REQUIRE(session->match().loaded());
    }
    trio.pump();
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        CAPTURE(session->host());
        REQUIRE(session->match().phase() == MatchSession::Phase::Running);
    }
    const auto before = trio.b.sentOn[*direct];
    for (s32 step = 0; step < 4; ++step) {
        trio.step();
    }
    CHECK(trio.b.sentOn[*direct] > before);
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        CHECK(session->phase() == OnlineSession::Phase::Active);
        CHECK(session->match().phase() == MatchSession::Phase::Running);
    }
    // The direct link closes: nobody fails, the host's relay carries on.
    trio.b.close(*direct);
    trio.pump();
    CHECK(trio.first.directLinks() == 0);
    CHECK(trio.second.directLinks() == 0);
    CHECK(trio.first.match().directLinks() == 0);
    CHECK(trio.second.match().directLinks() == 0);
    for (s32 step = 0; step < 4; ++step) {
        trio.step();
    }
    for (auto* session : {&trio.host, &trio.first, &trio.second}) {
        CHECK(session->phase() == OnlineSession::Phase::Active);
        CHECK(session->match().phase() == MatchSession::Phase::Running);
        CHECK(session->match().nativeConfirmed() >= 2);
    }
}
