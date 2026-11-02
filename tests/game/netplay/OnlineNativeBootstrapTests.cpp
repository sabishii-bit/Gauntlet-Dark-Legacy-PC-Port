#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestSupport.h"
#include "game/netplay/OnlineSession.h"
#include "game/online/NativePlaySetup.h"
#include "game/rollback/RollbackSessionHarness.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OnlineSession::Phase;
using Failure = OnlineSession::Failure;
using Connection = PacketTransport::Connection;
using Event = PacketTransport::Event;
using EventType = PacketTransport::EventType;
using Delivery = PacketTransport::Delivery;
using SendResult = PacketTransport::SendResult;

std::string peerId(u8 peer) {
    std::string name(32, static_cast<char>('a' + peer - 1));
    return name;
}
OnlineSession::NativeIdentity identity() {
    return {"same-executable-sha256", "windows-x64-msvc-14.44-fp-precise-v1", std::string(64, 'a'),
            std::string(64, 'b'), "en"};
}
bool bootstrap(std::span<const u8> bytes) {
    return bytes.size() >= 8 && bytes[0] == 'G' && bytes[1] == 'D' && bytes[2] == 'N' &&
           bytes[3] == 'B';
}

struct Network {
    struct Service final : RoomService {
        Network& network;
        u8 id;
        bool closed = false;
        bool holdStart = false;
        Service(Network& room, u8 peer) : network(room), id(peer) {}
        Update poll() override {
            auto snapshot = network.room;
            if (holdStart) {
                snapshot.started = false;
            }
            return {std::move(snapshot), peerId(id), {}, closed};
        }
        bool ready(u64 revision, bool value) override {
            if (revision != network.room.revision || network.room.started) {
                return false;
            }
            network.room.members[id - 1].ready = value;
            return true;
        }
        bool start(u64 revision) override {
            if (id != 1 || revision != network.room.revision || network.room.started ||
                !std::ranges::all_of(network.room.members,
                                     [](const auto& row) { return row.ready; })) {
                return false;
            }
            network.room.started = true;
            return true;
        }
        void leave() override { closed = true; }
    };
    struct Wire final : PeerTransport {
        Network& network;
        u8 id;
        std::map<Connection, std::string> peers;
        std::vector<Event> incoming;
        struct Sent {
            Connection connection;
            std::vector<u8> bytes;
            Delivery delivery;
        };
        std::vector<Sent> sent;
        std::function<SendResult(std::span<const u8>, Delivery)> filter;
        Wire(Network& room, u8 peer) : network(room), id(peer) {}
        bool configurePeer(std::string peer, bool host) override {
            return peer == peerId(id) && host == (id == 1);
        }
        bool authorizePeers(std::span<const std::string> /*peers*/) override { return true; }
        std::optional<Connection> connectPeer(const std::string& peer) override;
        std::optional<std::string> peer(Connection connection) const override {
            const auto found = peers.find(connection);
            return found == peers.end() ? std::nullopt : std::optional{found->second};
        }
        SendResult send(Connection connection, std::span<const u8> bytes,
                        Delivery delivery) override;
        std::vector<Event> poll() override { return std::exchange(incoming, {}); }
        std::optional<Statistics> statistics(Connection /*connection*/) const override {
            return std::nullopt;
        }
        void close(Connection /*connection*/) override {}
    };
    struct Client {
        Service service;
        Wire wire;
        OnlineSession session;
        test::RollbackSessionHarness inputs;
        Client(Network& room, u8 peer, u8 players)
            : service(room, peer), wire(room, peer), session(service, wire, peer == 1, players) {}
    };
    RoomSnapshot room;
    std::array<std::unique_ptr<Client>, 5> clients;
    std::array<std::unique_ptr<NativePlaySetup>, 5> setups;

    explicit Network(std::vector<u8> counts = {1, 1}) {
        room = {"ABCDEFGH", peerId(1), 1, false, {}, {4, 2, 2}};
        u8 seat = 0;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 peer = 1; peer <= static_cast<u8>(counts.size()); ++peer) {
            RoomMember member{peerId(peer), {}, false};
            for (u8 local = 0; local < counts[peer - 1]; ++local) {
                member.seats.push_back(seat++);
            }
            room.members.push_back(std::move(member));
            clients[peer] = std::make_unique<Client>(*this, peer, counts[peer - 1]);
            std::vector<CharacterProfile> profiles(counts[peer - 1]);
            for (auto& profile : profiles) {
                profile.name = "PLAYER";
                profile.character = peer - 1;
                profile.color = peer - 1;
            }
            REQUIRE(clients[peer]->session.select(profiles));
        }
        pump();
    }
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
    ~Network() {
        for (auto& client : clients) {
            if (client) {
                client->session.leave();
            }
        }
    }
    void pump(u32 rounds = 4, f64 seconds = 0) {
        for (u32 round = 0; round < rounds; ++round) {
            for (auto& client : clients) {
                if (client) {
                    client->session.update(seconds);
                }
            }
        }
    }
    void configure(OnlineSession::NativeRules rules = {}) {
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 peer = 1; peer <= static_cast<u8>(room.members.size()); ++peer) {
            REQUIRE(clients[peer]->session.configureNative(
                identity(), peer == 1 ? std::optional{rules} : std::nullopt));
        }
    }
    void start() {
        for (auto& client : clients) {
            if (client) {
                REQUIRE(client->session.ready());
            }
        }
        pump();
        REQUIRE(clients[1]->session.start());
        pump();
    }
    std::unique_ptr<NativePlaySetup> setup(u8 peer, const PlayOptions& options = {}) {
        auto& session = clients[peer]->session;
        const auto* contract = session.nativeContract();
        REQUIRE(contract);
        REQUIRE(session.party());
        GameConfig local;
        local.text.language = contract->identity.language;
        local.camera.horizontalFovDegrees = 45.0f + 10.0f * static_cast<f32>(peer);
        local.timing.gameplayFrameRate = 30U * peer;
        local.combat.autoMelee = !contract->rules.autoMelee;
        local.combat.autoActivateItems = !contract->rules.autoActivateItems;
        local.game.expandedShop = !contract->rules.expandedShop;
        const NativePlaySetup::Contract native{contract->rules.scene,
                                               contract->rules.autoMelee,
                                               contract->rules.autoActivateItems,
                                               contract->rules.expandedShop,
                                               contract->rules.autoAim,
                                               contract->identity.build,
                                               contract->identity.content,
                                               contract->identity.data};
        auto result = NativePlaySetup::create(local, session.room()->settings, *session.party(),
                                              options, native);
        REQUIRE(result);
        return result;
    }
    void confirm(u8 peer) {
        setups[peer] = setup(peer);
        REQUIRE(clients[peer]->session.confirmNativeSetup(setups[peer]->digest()));
    }
    void complete() {
        for (u32 round = 0; round < 12; ++round) {
            // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
            for (u8 peer = 1; peer <= static_cast<u8>(room.members.size()); ++peer) {
                if (clients[peer]->session.nativeSetupPending()) {
                    confirm(peer);
                }
            }
            pump(1);
        }
        for (auto& client : clients) {
            if (client) {
                REQUIRE(client->session.phase() == Phase::Active);
            }
        }
    }
    std::vector<u8> packet(u8 peer, u8 kind) const {
        const auto& sent = clients[peer]->wire.sent;
        const auto found = std::ranges::find_if(sent, [&](const auto& value) {
            return bootstrap(value.bytes) && value.bytes[5] == kind;
        });
        REQUIRE(found != sent.end());
        return found->bytes;
    }
};

std::optional<Connection> Network::Wire::connectPeer(const std::string& peer) {
    if (id == 1 || peer != peerId(1)) {
        return std::nullopt;
    }
    peers[99] = peer;
    auto& host = network.clients[1]->wire;
    host.peers[id] = peerId(id);
    host.incoming.push_back({EventType::Connected, id, {}, {}});
    incoming.push_back({EventType::Connected, 99, {}, {}});
    return 99;
}
SendResult Network::Wire::send(Connection connection, std::span<const u8> bytes,
                               Delivery delivery) {
    if (filter) {
        const auto result = filter(bytes, delivery);
        if (result != SendResult::Sent) {
            return result;
        }
    }
    const auto target = id == 1 ? static_cast<u8>(connection) : u8{1};
    if (!peers.contains(connection) || target >= network.clients.size() ||
        !network.clients[target]) {
        return SendResult::Disconnected;
    }
    sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
    network.clients[target]->wire.incoming.push_back({EventType::Message,
                                                      id == 1 ? Connection{99} : Connection{id},
                                                      {bytes.begin(), bytes.end()},
                                                      {}});
    return SendResult::Sent;
}
} // namespace

TEST_CASE("native production bootstrap agrees full setup before opening any match",
          "[online-native-bootstrap]") {
    const auto counts = GENERATE(std::vector<u8>{1, 1}, std::vector<u8>{2, 2},
                                 std::vector<u8>{1, 1, 1}, std::vector<u8>{1, 1, 1, 1});
    CAPTURE(counts);
    auto network = std::make_unique<Network>(counts);
    OnlineSession::NativeRules rules;
    rules.autoMelee = true;
    rules.autoActivateItems = false;
    rules.expandedShop = true;
    rules.startupDelay = 0;
    network->configure(rules);
    network->start();
    for (auto& client : network->clients) {
        if (!client) {
            continue;
        }
        REQUIRE(client->session.phase() == Phase::Starting);
        REQUIRE(client->session.nativeSetupPending());
        REQUIRE(client->session.nativeContract());
        CHECK(client->session.nativeContract()->rules == rules);
        CHECK(client->session.match().phase() == MatchSession::Phase::Offline);
        CHECK_FALSE(client->inputs.advance());
    }
    network->complete();
    const auto expected = network->setups[1]->digest();
    // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
    for (u8 peer = 1; peer <= static_cast<u8>(counts.size()); ++peer) {
        CHECK(network->setups[peer]->digest() == expected);
        CHECK(network->setups[peer]->config().camera.horizontalFovDegrees == 45.0f + 10.0f * peer);
        CHECK(network->setups[peer]->config().combat.autoMelee);
        CHECK_FALSE(network->setups[peer]->config().combat.autoActivateItems);
        CHECK(network->setups[peer]->config().game.expandedShop);
        CHECK(network->setups[peer]->config().game.mechanics.empty());
        CHECK(network->clients[peer]->session.match().nativeInputs());
        CHECK(network->clients[peer]->session.match().phase() == MatchSession::Phase::Lobby);
        for (const auto& sent : network->clients[peer]->wire.sent) {
            if (bootstrap(sent.bytes)) {
                CHECK(sent.delivery == Delivery::Reliable);
                CHECK(sent.bytes.size() <= OnlineSession::kNativePacketBytes);
            }
        }
    }
    REQUIRE(network->clients[1]->session.match().prepare(rules.scene));
    network->pump();
    for (auto& client : network->clients) {
        if (client) {
            REQUIRE(client->inputs.bind(client->session.match()));
            REQUIRE(client->session.match().loaded());
        }
    }
    network->pump();
    for (auto& client : network->clients) {
        if (!client) {
            continue;
        }
        auto& match = client->session.match();
        REQUIRE(match.phase() == MatchSession::Phase::Running);
        CHECK(match.nativeInputs());
        std::vector<InputCommand> frame;
        for (const auto seat : client->session.seats()) {
            InputCommand input;
            input.epoch = match.context().epoch;
            input.tick = client->inputs.sampleTick();
            input.seat = seat;
            input.grant = match.context().grants[seat];
            input.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
            frame.push_back(input);
        }
        REQUIRE(client->inputs.sample(frame));
        REQUIRE(client->session.flush());
    }
    network->pump();
    for (auto& client : network->clients) {
        if (!client) {
            continue;
        }
        const auto frame = client->inputs.advance();
        REQUIRE(frame);
        for (const auto& row : network->room.members) {
            for (const auto seat : row.seats) {
                CHECK(frame->commands[seat].pressedButtons ==
                      static_cast<u32>(CommandPress::UsePotion));
            }
        }
        REQUIRE(client->inputs.complete());
        CHECK(client->inputs.confirmedTick() == 1);
        CHECK_FALSE(client->inputs.advance()); // No new local input has been sampled.
    }
}

TEST_CASE("native bootstrap waits for every remote setup acknowledgment",
          "[online-native-bootstrap]") {
    auto network = std::make_unique<Network>(std::vector<u8>{1, 1, 1, 1});
    network->configure();
    network->start();
    for (u8 peer = 1; peer < 4; ++peer) {
        network->confirm(peer);
    }
    network->pump();
    for (auto& client : network->clients) {
        if (client) {
            CHECK(client->session.phase() == Phase::Starting);
        }
    }
    CHECK(network->clients[4]->session.nativeSetupPending());
    network->confirm(4);
    network->pump();
    for (auto& client : network->clients) {
        if (client) {
            CHECK(client->session.phase() == Phase::Active);
        }
    }
}

TEST_CASE("native bootstrap refuses executable runtime content data and language mismatch",
          "[online-native-bootstrap]") {
    const u32 field = GENERATE(0U, 1U, 2U, 3U, 4U);
    CAPTURE(field);
    auto network = std::make_unique<Network>();
    auto changed = identity();
    if (field == 0) {
        changed.build += "-other";
    }
    if (field == 1) {
        changed.runtime = "other-platform-or-numeric-runtime";
    }
    if (field == 2) {
        changed.content[0] = 'c';
    }
    if (field == 3) {
        changed.data[0] = 'c';
    }
    if (field == 4) {
        changed.language = "fr";
    }
    REQUIRE(network->clients[1]->session.configureNative(identity(), OnlineSession::NativeRules{}));
    REQUIRE(network->clients[2]->session.configureNative(changed));
    network->start();
    CHECK(network->clients[2]->session.phase() == Phase::Failed);
    CHECK(network->clients[2]->session.failure() == Failure::Protocol);
    CHECK(network->clients[2]->session.match().phase() == MatchSession::Phase::Offline);
    CHECK(network->clients[1]->session.phase() != Phase::Active);
}

TEST_CASE("native bootstrap verifies resolved options and profiles not only the offer",
          "[online-native-bootstrap]") {
    const bool hostFirst = GENERATE(false, true);
    auto network = std::make_unique<Network>();
    network->configure();
    network->start();
    PlayOptions different;
    different.position = Vec3{1, 2, 3};
    auto guest = network->setup(2, different);
    if (!hostFirst) {
        REQUIRE(network->clients[2]->session.confirmNativeSetup(guest->digest()));
    }
    network->confirm(1);
    network->pump();
    if (hostFirst) {
        CHECK_FALSE(network->clients[2]->session.confirmNativeSetup(guest->digest()));
    }
    CHECK(network->clients[2]->session.phase() == Phase::Failed);
    CHECK(network->clients[2]->session.failure() == Failure::Protocol);
    CHECK(network->clients[1]->session.phase() != Phase::Active);
}

TEST_CASE("native bootstrap preserves exact reliable packets under congestion",
          "[online-native-bootstrap]") {
    const u8 blockedKind = GENERATE(u8{0}, u8{1}, u8{2}, u8{3});
    CAPTURE(blockedKind);
    auto network = std::make_unique<Network>();
    network->configure();
    std::vector<u8> blocked;
    u32 attempts = 0;
    bool congested = true;
    const u8 sender = blockedKind == 2 ? u8{2} : u8{1};
    network->clients[sender]->wire.filter = [&](std::span<const u8> bytes, Delivery delivery) {
        if (bootstrap(bytes) && bytes[5] == blockedKind) {
            CHECK(delivery == Delivery::Reliable);
            if (blocked.empty()) {
                blocked.assign(bytes.begin(), bytes.end());
            }
            CHECK(std::ranges::equal(blocked, bytes));
            ++attempts;
            if (congested) {
                return SendResult::Congested;
            }
        }
        return SendResult::Sent;
    };
    network->start();
    for (u32 round = 0; round < 32; ++round) {
        for (u8 peer = 1; peer <= 2; ++peer) {
            if (network->clients[peer]->session.nativeSetupPending()) {
                network->confirm(peer);
            }
        }
        network->pump(1);
    }
    REQUIRE(attempts >= 16);
    CHECK(network->clients[2]->session.phase() == Phase::Starting);
    if (blockedKind == 3) {
        REQUIRE(network->clients[1]->session.phase() == Phase::Active);
        REQUIRE(network->clients[1]->session.match().prepare(OnlineSession::kNativeTowerScene));
        REQUIRE(network->clients[1]->session.flush());
        CHECK(std::ranges::none_of(network->clients[1]->wire.sent, [](const auto& sent) {
            return MatchControlPacket::decode(sent.bytes).has_value();
        }));
    } else {
        CHECK(network->clients[1]->session.phase() == Phase::Starting);
    }
    congested = false;
    network->complete();
    if (blockedKind == 3) {
        CHECK(network->clients[2]->session.match().phase() == MatchSession::Phase::Loading);
    }
}

TEST_CASE("native bootstrap is bounded strict and scoped to the authenticated room",
          "[online-native-bootstrap]") {
    const u32 mutation = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
    CAPTURE(mutation);
    auto network = std::make_unique<Network>();
    network->configure();
    network->start();
    auto bytes = network->packet(1, 0);
    if (mutation == 0) {
        ++bytes[4]; // An unsupported version, independent of the current protocol value.
    }
    if (mutation == 1) {
        bytes[5] = 255;
    }
    if (mutation == 2) {
        bytes[6] = 0;
    }
    if (mutation == 3) {
        bytes[7] = 1;
    }
    if (mutation == 4) {
        bytes[8] = bytes[8] == 'a' ? 'b' : 'a';
    }
    if (mutation == 5) {
        bytes[76] = 8;
    }
    if (mutation == 6) {
        bytes.pop_back();
    }
    if (mutation == 7) {
        bytes.resize(OnlineSession::kNativePacketBytes + 1);
    }
    network->clients[2]->wire.incoming.push_back({EventType::Message, 99, bytes, {}});
    network->clients[2]->session.update(0);
    CHECK(network->clients[2]->session.phase() == Phase::Failed);
    CHECK(network->clients[2]->session.failure() == Failure::Protocol);
    CHECK(network->clients[2]->session.match().phase() == MatchSession::Phase::Offline);
}

TEST_CASE("native bootstrap retains early offer and accepts only identical duplicates",
          "[online-native-bootstrap]") {
    auto network = std::make_unique<Network>();
    network->configure();
    network->clients[2]->service.holdStart = true;
    network->start();
    REQUIRE(network->clients[1]->session.phase() == Phase::Starting);
    REQUIRE(network->clients[2]->session.phase() == Phase::Lobby);
    CHECK(network->clients[2]->session.nativeContract() == nullptr);
    const auto offer = network->packet(1, 0);
    for (u32 repeat = 0; repeat < 12; ++repeat) {
        network->clients[2]->wire.incoming.push_back({EventType::Message, 99, offer, {}});
    }
    network->clients[2]->session.update(0);
    CHECK(network->clients[2]->session.phase() == Phase::Lobby);
    network->clients[2]->service.holdStart = false;
    network->pump();
    REQUIRE(network->clients[2]->session.nativeContract());
    network->complete();
    const auto acknowledge = network->packet(2, 2);
    const auto seal = network->packet(1, 1);
    const auto commit = network->packet(1, 3);
    for (const auto& bytes : {offer, seal, commit}) {
        network->clients[2]->wire.incoming.push_back({EventType::Message, 99, bytes, {}});
    }
    network->clients[1]->wire.incoming.push_back({EventType::Message, 2, acknowledge, {}});
    network->pump();
    CHECK(network->clients[1]->session.phase() == Phase::Active);
    CHECK(network->clients[2]->session.phase() == Phase::Active);
    auto changed = offer;
    changed[76] ^= 1;
    network->clients[2]->wire.incoming.push_back({EventType::Message, 99, changed, {}});
    network->clients[2]->session.update(0);
    CHECK(network->clients[2]->session.phase() == Phase::Failed);
}

TEST_CASE("online bootstrap cannot start an unconfigured peer or silently downgrade",
          "[online-native-bootstrap]") {
    const bool nativeHost = GENERATE(false, true);
    auto network = std::make_unique<Network>();
    REQUIRE(network->clients[nativeHost ? 1 : 2]->session.configureNative(
        identity(), nativeHost ? std::optional{OnlineSession::NativeRules{}} : std::nullopt));
    const auto unconfigured = nativeHost ? u8{2} : u8{1};
    CHECK_FALSE(network->clients[unconfigured]->session.ready());
    CHECK_FALSE(network->clients[1]->session.start());
    for (const auto& client : network->clients) {
        if (client) {
            CHECK(client->session.match().phase() == MatchSession::Phase::Offline);
        }
    }
    // Even a service that claims the room has started cannot bypass identity.
    network->room.started = true;
    network->pump();
    CHECK(network->clients[unconfigured]->session.phase() == Phase::Failed);
    CHECK(network->clients[unconfigured]->session.failure() == Failure::Protocol);
    CHECK_FALSE(network->clients[unconfigured]->session.match().nativeInputs());
}

TEST_CASE("native bootstrap authenticates explicit development scene identifiers",
          "[online-native-bootstrap]") {
    auto network = std::make_unique<Network>();
    auto rules = OnlineSession::NativeRules{};
    rules.scene = 7 * 256 + 2;
    network->configure(rules);
    network->start();
    network->complete();
    REQUIRE(network->clients[1]->session.match().prepare(rules.scene));
    network->pump();
    CHECK(network->clients[2]->session.match().context().scene == rules.scene);
    for (const u32 invalid : {0U, 256U, 65536U, 0xFFFFFFFFU}) {
        rules.scene = invalid;
        CHECK_FALSE(rules.valid());
    }
}

TEST_CASE("native bootstrap refuses invalid setup contracts and late reconfiguration",
          "[online-native-bootstrap]") {
    auto network = std::make_unique<Network>();
    auto invalid = identity();
    invalid.runtime.clear();
    CHECK_FALSE(
        network->clients[1]->session.configureNative(invalid, OnlineSession::NativeRules{}));
    invalid = identity();
    invalid.data[0] = 'G';
    CHECK_FALSE(
        network->clients[1]->session.configureNative(invalid, OnlineSession::NativeRules{}));
    auto rules = OnlineSession::NativeRules{};
    rules.scene = 256;
    CHECK_FALSE(network->clients[1]->session.configureNative(identity(), rules));
    rules = {};
    rules.startupDelay = 7;
    CHECK_FALSE(network->clients[1]->session.configureNative(identity(), rules));
    CHECK_FALSE(network->clients[1]->session.configureNative(identity()));
    CHECK_FALSE(
        network->clients[2]->session.configureNative(identity(), OnlineSession::NativeRules{}));
    network->configure();
    CHECK_FALSE(network->clients[1]->session.confirmNativeSetup(std::string(64, 'a')));
    network->start();
    CHECK_FALSE(network->clients[2]->session.configureNative(identity()));
    CHECK_FALSE(network->clients[2]->session.confirmNativeSetup("not-a-digest"));
    network->complete();
    REQUIRE(network->clients[1]->session.match().prepare(1));
    CHECK_FALSE(network->clients[1]->session.flush());
    CHECK(network->clients[1]->session.phase() == Phase::Failed);
}

TEST_CASE("native bootstrap missing confirmation and disconnected peers fail closed",
          "[online-native-bootstrap]") {
    const u32 failure = GENERATE(0U, 1U, 2U);
    CAPTURE(failure);
    auto network = std::make_unique<Network>();
    network->configure();
    network->start();
    if (failure == 1) {
        network->clients[1]->wire.incoming.push_back({EventType::Disconnected, 2, {}, {}});
        network->clients[1]->session.update(0);
    } else if (failure == 2) {
        network->clients[1]->wire.filter = [](std::span<const u8> bytes, Delivery) {
            return bootstrap(bytes) ? SendResult::Invalid : SendResult::Sent;
        };
        network->confirm(1);
        network->pump(1);
    } else {
        network->clients[1]->session.update(OnlineSession::kStartTimeout);
    }
    CHECK(network->clients[1]->session.phase() == Phase::Failed);
    CHECK(network->clients[1]->session.failure() ==
          (failure != 0 ? Failure::Transport : Failure::Timeout));
    CHECK(network->clients[1]->session.match().phase() == MatchSession::Phase::Offline);
}

TEST_CASE("native bootstrap packets keep their byte layout", "[online-native-bootstrap]") {
    auto network = std::make_unique<Network>();
    OnlineSession::NativeRules rules;
    rules.autoMelee = true;
    rules.autoActivateItems = false;
    rules.expandedShop = true;
    rules.autoAim = true;
    rules.startupDelay = 3;
    network->configure(rules);
    network->start();
    const auto offer = network->packet(1, 0);
    constexpr std::string_view kOffer =
        "47444e42" // magic
        "02000200" // version, offer, input mode, zero
        "6139346437306565643235356361376561383231323235313964333665303633" // the room digest
        "3861653334343766343262393731313163303266623137376261663762303732"
        "010e0000"                                     // scene
        "0d030100"                                     // rules, delay, setup, zero
        "16000000"                                     // build length
        "73616d652d65786563757461626c652d736861323536" // build
        "24000000"                                     // runtime length
        "77696e646f77732d7836342d6d7376632d31342e34342d66702d707265636973" // runtime
        "652d7631"
        "40000000"                                                         // content length
        "6161616161616161616161616161616161616161616161616161616161616161" // content
        "6161616161616161616161616161616161616161616161616161616161616161"
        "40000000"                                                         // data length
        "6262626262626262626262626262626262626262626262626262626262626262" // data
        "6262626262626262626262626262626262626262626262626262626262626262"
        "02000000" // language length
        "656e";    // "en"
    CHECK(test::hexBytes(offer) == kOffer);
    network->complete();
    const auto setup = network->setups[1]->digest();
    const auto scope = std::span{offer}.subspan(8, 64);
    for (const auto& [peer, kind] :
         {std::pair{u8{1}, u8{1}}, std::pair{u8{2}, u8{2}}, std::pair{u8{1}, u8{3}}}) {
        std::vector<u8> expected{'G', 'D', 'N', 'B', 2, kind, 2, 0};
        expected.insert(expected.end(), scope.begin(), scope.end());
        expected.insert(expected.end(), setup.begin(), setup.end());
        CHECK(network->packet(peer, kind) == expected);
    }
}
