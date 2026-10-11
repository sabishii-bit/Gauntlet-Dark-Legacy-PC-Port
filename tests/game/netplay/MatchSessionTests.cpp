#include <algorithm>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

#include "game/screens/MatchInputs.h"
#include "game/screens/PartyMotion.h"
#include "game/world/CameraMovementLimit.h"

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
    usize budget = std::numeric_limits<usize>::max();
    std::vector<Packet> sent;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        if (result == SendResult::Sent) {
            if (budget == 0) {
                return SendResult::Congested;
            }
            --budget;
            sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        }
        return result;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return std::nullopt;
    }
    void close(Connection /*connection*/) override {}
};

struct Room {
    MatchOwners owners;
    std::array<MatchSession, 5> sessions;
    std::array<Wire, 5> wires;
    explicit Room(MatchOwners roster = {1, 2, 3, 4}) : owners(roster) {
        std::vector<MatchLink> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, static_cast<u64>(peer - 1)});
                const std::array server{MatchLink{1, 99}};
                REQUIRE(sessions[peer].open(peer, owners, server));
            }
        }
        REQUIRE(sessions[1].open(1, owners, links));
    }
    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
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
    void pump() {
        for (u8 pass = 0; pass < 5; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (!present(peer)) {
                    continue;
                }
                sessions[peer].flush(wires[peer]);
                const auto sent = std::move(wires[peer].sent);
                wires[peer].sent.clear();
                for (const auto& packet : sent) {
                    const auto target = peer == 1 ? packet.connection + 1 : 1;
                    const auto sender = peer == 1 ? 99 : peer - 1;
                    const auto admitted = sessions[target].receive(sender, packet.bytes);
                    REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
                }
            }
        }
    }
    void ready(MatchTransition transition = MatchTransition::Start, u32 scene = 7) {
        const auto profiles = party();
        REQUIRE(sessions[1].prepare(scene, transition,
                                    transition == MatchTransition::Travel ? &profiles : nullptr));
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Loading);
                REQUIRE(sessions[peer].loaded());
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
    const auto bytes = MatchControlPacket::encode({kind, stop, session.context(), votes});
    REQUIRE(bytes);
    return *bytes;
}
CombatSnapshot snapshot(const MatchSession& session) {
    CombatSnapshot result;
    result.motion.epoch = session.context().epoch;
    result.motion.tick = session.tick() - 1;
    result.motion.cameraContinuity = 1;
    for (usize seat = 0; seat < 4; ++seat) {
        if (session.context().owners[seat] != 0) {
            result.motion.players[seat] = SeatMotion{session.context().grants[seat], 1, {}, 0};
            result.players[seat] =
                PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, 0, 1}};
        }
    }
    return result;
}

TEST_CASE("match control has an exact bounded versioned encoding", "[netplay][match-session]") {
    Room room;
    REQUIRE(room.sessions[1].prepare(7));
    for (u8 kind = 0; kind <= 6; ++kind) {
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
        CHECK(decoded->movieSkipVotes == votes);
        CHECK(bytes[4] == 3);
        for (usize size = 0; size < bytes.size(); ++size) {
            CHECK_FALSE(MatchControlPacket::decode(std::span(bytes).first(size)));
        }
        auto trailing = bytes;
        trailing.push_back(0);
        CHECK_FALSE(MatchControlPacket::decode(trailing));
        for (const usize index : {0U, 4U, 5U, 6U, 7U, 20U, 22U, 23U, 24U}) {
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
    CHECK(host.movieSkipVotes() == 5);
    CHECK(guest.movieSkipVotes() == 5);
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
    CHECK(host.tick() == 0);
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    room.pump();
    const auto oldVote = message(guest, MatchControlKind::MovieSkipVote, MatchStop::None, 8);
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    REQUIRE(host.requestPause());
    room.pump();
    room.ready(MatchTransition::Resume);
    CHECK(host.movieSkipVotes() == 0);
    CHECK(guest.movieSkipVotes() == 0);
    CHECK_FALSE(host.requestMovieSkip(0));
    const auto profiles = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
    room.pump();
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    CHECK(host.movieSkipVotes() == 0);
    REQUIRE(guest.requestMovieSkip(2));
    room.pump();
    CHECK(host.movieSkipVotes() == 4);
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
    CHECK(host.movieSkipVotes() == 0);
    CHECK_FALSE(host.requestMovieSkip(2));
    for (const u8 mask : std::array<u8, 4>{0, 4, 16, 255}) {
        CHECK_FALSE(MatchControlPacket::encode(
            {MatchControlKind::MovieSkipVote, MatchStop::None, host.context(), mask}));
    }
    CHECK_FALSE(
        MatchControlPacket::encode({MatchControlKind::Ready, MatchStop::None, host.context(), 1}));
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
    CHECK_FALSE(session.advance());
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

TEST_CASE("travel waits for every profile and rejects stale or forged progression",
          "[netplay][match-session][party-checkpoint]") {
    Room room({1, 2, 2, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto oldContext = host.context();
    auto party = room.party();
    party[1]->gold = 7654;
    party[1]->progress.inventory.keys = 7;
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel));
    auto missing = party;
    missing[2].reset();
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &missing));
    CHECK(host.context() == oldContext);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    REQUIRE(host.travelParty());
    REQUIRE(host.loaded());
    auto& wire = room.wires[1];
    wire.budget = 1; // Prepare arrives, then each profile in a separate batch.
    host.flush(wire);
    REQUIRE(wire.sent.size() == 1);
    REQUIRE(guest.receive(99, wire.sent.back().bytes) == Admission::Accepted);
    wire.sent.clear();
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(guest.loaded());
    CHECK_FALSE(host.advance());

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
            CHECK_FALSE(guest.loaded());
        }
        auto changed = *PartyCheckpointPacket::decode(packet.bytes);
        ++changed.profile.gold;
        CHECK(guest.receive(99, *PartyCheckpointPacket::encode(changed)) == Admission::Invalid);
    }
    REQUIRE(guest.travelParty());
    CHECK((*guest.travelParty())[1]->gold == 7654);
    CHECK((*guest.travelParty())[1]->progress.inventory.keys == 7);
    REQUIRE(guest.loaded());
    wire.budget = 100;
    room.pump();
    CHECK(host.phase() == Phase::Running);
    CHECK(guest.phase() == Phase::Running);
    REQUIRE(host.requestPause());
    room.pump();
    REQUIRE(host.prepare(8, MatchTransition::Resume));
    room.pump();
    CHECK(guest.receive(99, *encoded) == Admission::Stale);
    CHECK_FALSE(guest.travelParty());
}

TEST_CASE("travel preparation is bounded under congestion and missing profiles time out",
          "[netplay][match-session][party-checkpoint]") {
    Room room;
    room.ready();
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
    guest.update(MatchSession::kLoadTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::LoadTimeout);
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(guest.loaded());
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
    REQUIRE(host.loaded());
    room.pump();
    for (u8 peer = 2; peer < 4; ++peer) {
        if (room.present(peer)) {
            REQUIRE(room.sessions[peer].loaded());
            CHECK_FALSE(room.sessions[peer].loaded());
        }
    }
    room.pump();
    CHECK_FALSE(host.advance());
    CHECK_FALSE(MatchInputs::sample(host, {}));
    CHECK(host.tick() == 0);
    auto& slow = room.sessions[4];
    CHECK(slow.receive(99, message(host, MatchControlKind::Commit)) == Admission::WrongPhase);
    REQUIRE(slow.loaded());
    room.pump();
    REQUIRE(host.phase() == Phase::Running);
    REQUIRE(host.advance());
    CHECK(host.tick() == 1);
    CHECK_FALSE(slow.advance());
    CHECK_FALSE(slow.prepare(8));
    CHECK_FALSE(slow.publish(snapshot(host)));
}

TEST_CASE("unreliable gameplay may overtake Commit or trail Pause without bypassing either",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.prepare(7));
    room.pump();
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    REQUIRE(host.receive(1, message(guest, MatchControlKind::Ready)) == Admission::Accepted);
    REQUIRE(host.phase() == Phase::Running);
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    Wire wire;
    host.flush(wire);
    REQUIRE(wire.sent.size() >= 2);
    for (const auto& packet : wire.sent) {
        if (packet.delivery == PacketTransport::Delivery::Unreliable) {
            CHECK(guest.receive(99, packet.bytes) == Admission::Stale);
            CHECK(guest.receive(123, packet.bytes) == Admission::WrongPeer);
        }
    }
    CHECK(guest.phase() == Phase::Loading);
    CHECK_FALSE(guest.advance());
    CHECK_FALSE(guest.playback().latest());
    for (const auto& packet : wire.sent) {
        if (packet.delivery == PacketTransport::Delivery::Reliable) {
            REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
        }
    }
    REQUIRE(guest.phase() == Phase::Running);
    wire.sent.clear();
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    host.flush(wire);
    for (const auto& packet : wire.sent) {
        REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
    }
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick == 1);

    REQUIRE(MatchInputs::sample(guest, {}));
    Wire inputs;
    guest.flush(inputs);
    REQUIRE(host.requestPause());
    for (const auto& packet : inputs.sent) {
        if (packet.delivery == PacketTransport::Delivery::Unreliable) {
            CHECK(host.receive(1, packet.bytes) == Admission::Stale);
        }
    }
    REQUIRE(guest.receive(99, message(host, MatchControlKind::Pause)) == Admission::Accepted);
    for (const auto& packet : wire.sent) {
        CHECK(guest.receive(99, packet.bytes) == Admission::Stale);
    }
    CHECK(host.phase() == Phase::Paused);
    CHECK(guest.phase() == Phase::Paused);
    CHECK_FALSE(host.advance());
    CHECK(guest.playback().latest()->motion.tick == 1);
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
    REQUIRE(MatchInputs::sample(guest, local));
    REQUIRE(MatchInputs::sample(host, local));
    CHECK_FALSE(MatchInputs::sample(host, local));
    room.pump();
    auto frame = MatchInputs::advance(host);
    REQUIRE(frame);
    CHECK((*frame)[0].throwPotion);
    CHECK((*frame)[2].move.direction == Vec2{-1, 0});
    CHECK((*frame)[2].aimDirection == local[1].aimDirection);
    CHECK((*frame)[1].move.magnitude == 0);
    for (u8 tick = 1; tick <= guest.context().inputLead; ++tick) {
        frame = MatchInputs::advance(host);
        REQUIRE(frame);
    }
    CHECK((*frame)[1].throwPotion);
    CHECK((*frame)[3].move.direction == Vec2{-1, 0});
    CHECK((*frame)[3].aimDirection == local[1].aimDirection);
    for (const auto& input : *frame) {
        CHECK(input.menu.devices == nullptr);
        CHECK(input.menu.typed.empty());
        CHECK_FALSE(input.menu.pointer);
    }
    CHECK_FALSE((*MatchInputs::advance(host))[1].throwPotion);
}

TEST_CASE("spoofed trailing input cannot partially apply an otherwise valid command",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    InputCommand input;
    input.epoch = host.context().epoch;
    input.grant = host.context().grants[1];
    input.seat = 1;
    input.pressedButtons = static_cast<u32>(CommandPress::ThrowPotion);
    auto spoof = input;
    spoof.seat = 0;
    const std::array commands{input, spoof};
    const auto packet = InputPacket::encode(commands);
    REQUIRE(packet);
    CHECK(host.receive(1, *packet) == Admission::WrongPeer);
    const auto frame = host.advance();
    REQUIRE(frame);
    CHECK_FALSE((*frame)[1].pressed(CommandPress::ThrowPotion));
    CHECK_FALSE((*frame)[0].pressed(CommandPress::ThrowPotion));
    input.tick = host.tick() + InputTimeline::kMaxAhead + 1;
    const auto future = InputPacket::encode(std::span(&input, 1));
    REQUIRE(future);
    CHECK(host.receive(1, *future) == Admission::Invalid);
}

TEST_CASE("simultaneous guest pause requests do not terminate the shared match",
          "[netplay][match-session][multiplayer]") {
    Room room;
    room.ready();
    for (u8 peer = 2; peer <= 4; ++peer) {
        REQUIRE(room.sessions[peer].requestPause());
    }
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.sessions[peer].phase() == Phase::Paused);
        CHECK(room.sessions[peer].stopReason() == MatchStop::None);
        CHECK_FALSE(room.sessions[peer].advance());
    }
    room.ready(MatchTransition::Resume);
    CHECK(room.sessions[1].advance());
}

TEST_CASE("a stalled host bounds guest input lead and coalesces pending actions",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    SessionInputs::Frame input;
    for (u32 tick = 0; tick < 600; ++tick) {
        input[0].throwPotion = tick == 200;
        input[1].selector.up = tick == 300;
        input[0].attack = tick < 500;
        REQUIRE(MatchInputs::sample(guest, input));
        room.pump(); // These packets used to exceed kMaxAhead and fail admission.
    }
    CHECK_FALSE(guest.inputReady());
    CHECK(guest.inputTick() == guest.context().inputLead + MatchSession::kInputSlack + 1);
    const u64 pendingTick = guest.inputTick();
    CHECK(host.tick() == 0);
    CHECK(host.phase() == Phase::Running);

    // New snapshots let the guest continue. The stalled period's discrete presses
    // survive, but its old held attack and movement do not replay after release.
    input = {};
    input[1].move = {{-1, 0}, 1};
    std::array<u32, 4> potions{};
    std::array<u32, 4> selections{};
    for (u32 tick = 0; tick < 100; ++tick) {
        REQUIRE(MatchInputs::sample(guest, input));
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        for (usize seat = 0; seat < 4; ++seat) {
            potions[seat] += (*frame)[seat].pressed(CommandPress::ThrowPotion) ? 1U : 0U;
            selections[seat] += (*frame)[seat].pressed(CommandPress::SelectorUp) ? 1U : 0U;
        }
        if (tick >= pendingTick) {
            CHECK_FALSE((*frame)[1].held(CommandHeld::Attack));
            CHECK((*frame)[3].direction == Vec2{-1, 0});
        }
        if (tick % 3 == 0) {
            REQUIRE(host.publish(snapshot(host)));
        }
        room.pump();
    }
    CHECK(potions == std::array<u32, 4>{0, 1, 0, 0});
    CHECK(selections == std::array<u32, 4>{0, 0, 0, 1});
    CHECK(guest.inputTick() > pendingTick);
}

TEST_CASE("pending guest presses are atomic and cannot leak across pause or travel",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    while (guest.inputReady()) {
        REQUIRE(MatchInputs::sample(guest, {}));
    }
    SessionInputs::Frame input;
    input[0].throwPotion = true;
    REQUIRE(MatchInputs::sample(guest, input));
    SECTION("pause discards pending gameplay") {
        REQUIRE(host.requestPause());
        room.pump();
        room.ready(MatchTransition::Resume);
    }
    SECTION("travel discards pending gameplay") {
        room.ready(MatchTransition::Travel, 8);
    }
    SECTION("invalid frame cannot poison pending input") {
        // This section retains the valid pending potion, but rejects a spoofed
        // attack on another seat before changing any of the buffered presses.
        auto command = SessionInputs::command(input[0], guest.context().epoch, guest.inputTick(),
                                              guest.context().grants[1], 1);
        command.pressedButtons = static_cast<u32>(CommandPress::Attack);
        auto invalid = command;
        invalid.seat = 0;
        const std::array frame{command, invalid};
        CHECK_FALSE(guest.sample(frame));
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
        REQUIRE(guest.inputReady());
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        u32 potions = 0;
        for (u32 tick = 0; tick < 30; ++tick) {
            const auto received = host.advance();
            REQUIRE(received);
            CHECK_FALSE((*received)[1].pressed(CommandPress::Attack));
            potions += (*received)[1].pressed(CommandPress::ThrowPotion) ? 1U : 0U;
        }
        CHECK(potions == 1);
        return;
    }
    REQUIRE(guest.inputReady());
    for (u32 tick = 0; tick < 30; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK_FALSE((*frame)[1].pressed(CommandPress::ThrowPotion));
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
    }
}

TEST_CASE("guest clock skew stays bounded during a long running match",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    u32 moving = 0;
    SessionInputs::Frame input;
    input[0].move = {{1, 0}, 1};
    for (u32 tick = 0; tick < 3600; ++tick) {
        // A deliberately exaggerated 10% faster guest clock, including a long
        // burst with no completed snapshots and a guest that later falls behind.
        if (tick % 10 != 0 || tick < 2400) {
            REQUIRE(MatchInputs::sample(guest, input));
        }
        if (tick % 10 == 0 && tick < 2400) {
            REQUIRE(MatchInputs::sample(guest, input));
        }
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        moving += (*frame)[1].magnitude > 0 ? 1U : 0U;
        if (tick % 3 == 0 && (tick < 1200 || tick > 1500)) {
            REQUIRE(host.publish(snapshot(host)));
        }
        room.pump();
        CHECK(guest.inputTick() < host.tick() + InputTimeline::kMaxAhead);
    }
    CHECK(moving > 3200);
    CHECK(guest.phase() == Phase::Running);
}

TEST_CASE("pause resume and travel isolate queued actions and replicated state by epoch",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto oldReady = message(guest, MatchControlKind::Ready);
    SessionInputs::Frame input;
    input[0].throwPotion = true;
    input[0].attack = true;
    REQUIRE(MatchInputs::sample(guest, input));
    room.pump();
    REQUIRE(host.advance());
    const auto oldState = snapshot(host);
    REQUIRE(host.publish(oldState));
    room.pump();
    REQUIRE(guest.playback().latest());
    REQUIRE(guest.requestPause());
    REQUIRE(guest.requestPause());
    CHECK(guest.queuedControls() == 1);
    room.pump();
    CHECK(host.phase() == Phase::Paused);
    CHECK(guest.phase() == Phase::Paused);
    const auto before = host.tick();
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.publish(oldState));
    CHECK_FALSE(MatchInputs::sample(guest, input));
    host.update(100);
    CHECK(host.tick() == before);
    CHECK_FALSE(host.prepare(8, MatchTransition::Resume));
    room.ready(MatchTransition::Resume);
    CHECK(host.context().epoch == oldState.motion.epoch + 1);
    CHECK_FALSE(guest.playback().latest());
    CHECK(host.receive(1, oldReady) == Admission::Stale);
    const auto packets = CombatReplica::packets(oldState);
    REQUIRE(packets);
    CHECK(guest.receive(99, packets->front()) == Admission::Stale);
    for (u8 tick = 0; tick < 20; ++tick) {
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK_FALSE((*frame)[1].held(CommandHeld::Attack));
        CHECK_FALSE((*frame)[1].pressed(CommandPress::ThrowPotion));
    }
    room.ready(MatchTransition::Travel, 8);
    CHECK(host.tick() == 0);
    CHECK(guest.context().scene == 8);
    CHECK(guest.context().transition == MatchTransition::Travel);
}

TEST_CASE("pause and resume preserve reliable order when the control channel is congested",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    room.wires[1].result = PacketTransport::SendResult::Congested;
    REQUIRE(host.requestPause());
    REQUIRE(host.prepare(7, MatchTransition::Resume));
    host.flush(room.wires[1]);
    CHECK(host.queuedControls() == 2);
    CHECK(room.sessions[2].phase() == Phase::Running);
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(host.queuedControls() == 0);
    CHECK(room.sessions[2].phase() == Phase::Loading);
    CHECK(room.sessions[2].context() == host.context());
}

TEST_CASE("unsent snapshots and input are bounded latest state not a reliable backlog",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    room.wires[1].result = PacketTransport::SendResult::Congested;
    room.wires[2].result = PacketTransport::SendResult::Congested;
    for (usize tick = 0; tick < 100; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        host.flush(room.wires[1]);
        guest.flush(room.wires[2]);
    }
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.wires[2].result = PacketTransport::SendResult::Sent;
    guest.flush(room.wires[2]);
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(room.wires[2].sent.front().delivery == PacketTransport::Delivery::Unreliable);
    const auto commands = InputPacket::decode(room.wires[2].sent.front().bytes);
    REQUIRE(commands);
    CHECK(commands->size() == InputHistory::kTicks);
    host.flush(room.wires[1]);
    const auto latest = CombatReplica::packets(snapshot(host));
    REQUIRE(latest);
    REQUIRE(room.wires[1].sent.size() == latest->size());
    for (usize i = 0; i < latest->size(); ++i) {
        CHECK(room.wires[1].sent[i].bytes == (*latest)[i]);
        CHECK(room.wires[1].sent[i].delivery == PacketTransport::Delivery::Unreliable);
    }
}

TEST_CASE("a congested partial snapshot completes while newer captures coalesce",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    auto& wire = room.wires[1];
    const auto dense = [&] {
        auto state = snapshot(host);
        state.geometry = SceneGeometry{1, 806, 0, {}};
        for (u32 i = 0; i < 806; ++i) {
            const u32 row = i / 31;
            const Vec3 position{static_cast<f32>(i % 31), static_cast<f32>(host.tick()) / 60,
                                static_cast<f32>(row)};
            state.geometry->objects.push_back({i, 1, glm::translate(Mat4{1}, position), 1, true});
        }
        return state;
    };
    REQUIRE(host.advance());
    const auto first = dense();
    const auto packets = CombatReplica::packets(first, SnapshotBlock::Compression::Automatic,
                                                CombatReplica::Recovery::SingleLoss);
    REQUIRE(packets);
    REQUIRE(packets->size() > 2);
    REQUIRE(host.publish(first));
    const auto drain = [&] {
        host.flush(wire);
        for (const auto& packet : wire.sent) {
            const auto admission = guest.receive(99, packet.bytes);
            REQUIRE((admission == Admission::Accepted || admission == Admission::Stale));
        }
        wire.sent.clear();
    };
    // Only one fragment fits per capture interval: replacing unfinished batches
    // here used to send first fragments forever without one complete checkpoint.
    for (usize tick = 0; tick < packets->size() * 2 + 5; ++tick) {
        REQUIRE(host.advance());
        REQUIRE(host.publish(dense()));
        wire.budget = 1;
        drain();
    }
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick > 0);
    const auto before = guest.playback().latest()->motion.tick;
    // Hundreds of further captures replace one pending batch, not the in-flight
    // batch and not a growing list of stale snapshots.
    wire.budget = 0;
    for (usize tick = 0; tick < 200; ++tick) {
        REQUIRE(host.advance());
        REQUIRE(host.publish(dense()));
        drain();
    }
    wire.budget = std::numeric_limits<usize>::max();
    host.flush(wire);
    const auto last = CombatReplica::packets(dense(), SnapshotBlock::Compression::Automatic,
                                             CombatReplica::Recovery::SingleLoss);
    REQUIRE(last);
    CHECK(wire.sent.size() <= packets->size() + last->size());
    drain();
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick > before);
    CHECK(CombatPacket::encode(*guest.playback().latest()) == CombatPacket::encode(dense()));
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

TEST_CASE("departure stops every remaining machine without stale input or state revival",
          "[netplay][match-session]") {
    Room room;
    room.ready();
    auto& host = room.sessions[1];
    const auto oldPrepare = message(host, MatchControlKind::Prepare);
    SECTION("running") {}
    SECTION("paused") {
        REQUIRE(host.requestPause());
        room.pump();
    }
    SECTION("loading next stage") {
        const auto profiles = room.party();
        REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
        room.pump();
    }
    host.disconnected(1); // remote peer 2, NOT host peer 1
    room.sessions[2].disconnected(99);
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.sessions[peer].phase() == Phase::Stopped);
        CHECK(room.sessions[peer].stopReason() == MatchStop::Disconnected);
        CHECK_FALSE(room.sessions[peer].advance());
        CHECK_FALSE(room.sessions[peer].playback().latest());
        CHECK_FALSE(MatchInputs::sample(room.sessions[peer], {}));
    }
    CHECK(room.sessions[3].receive(99, oldPrepare) == Admission::WrongPhase);
}

TEST_CASE("transport send failure terminates loading instead of pretending a start succeeded",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    room.wires[1].result = PacketTransport::SendResult::Disconnected;
    host.flush(room.wires[1]);
    CHECK(host.phase() == Phase::Stopped);
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.loaded());
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
        if (tick == 90) {
            REQUIRE(guest.requestPause());
            room.pump();
            const auto held = CombatPacket::encode(*guest.playback().latest());
            for (u32 paused = 0; paused < 100; ++paused) {
                CHECK_FALSE(MatchInputs::advance(host));
                CHECK_FALSE(MatchInputs::sample(guest, {}));
                CHECK(CombatPacket::encode(*guest.playback().latest()) == held);
            }
            room.ready(MatchTransition::Resume);
        }
        SessionInputs::Frame local;
        for (auto& input : local) {
            input.move = {{1, 0}, 1};
            input.usePotion = tick == 0 || tick == 90;
            input.selector.up = input.usePotion;
        }
        REQUIRE(MatchInputs::sample(host, local));
        REQUIRE(MatchInputs::sample(guest, local));
        room.pump();
        const auto inputs = MatchInputs::advance(host);
        REQUIRE(inputs);
        CHECK_FALSE(MatchInputs::advance(guest));
        const auto moved = PartyMotion::step(players, *inputs, false, camera.yaw(), 1, 1.0f / 60,
                                             collision, events);
        camera.update(moved, {}, range, view, 1.0f / 60);
        auto state = snapshot(host);
        state.motion.camera = camera.camera();
        state.motion.aspect = view.aspect;
        state.motion.horizontalFov = view.horizontalFov;
        for (usize seat = 0; seat < players.size(); ++seat) {
            state.motion.players[seat]->position = players[seat].actor.position();
            state.motion.players[seat]->yaw = players[seat].actor.yaw();
        }
        REQUIRE(host.publish(state));
        room.pump();
        REQUIRE(guest.playback().latest());
        CHECK(CombatPacket::encode(*guest.playback().latest()) == CombatPacket::encode(state));
    }
    for (usize seat = 0; seat < players.size(); ++seat) {
        CHECK(potions[seat] == 2);
        CHECK(toggles[seat] == 2);
        CHECK(glm::length(players[seat].actor.position() - starts[seat]) > 1);
    }
}
} // namespace
