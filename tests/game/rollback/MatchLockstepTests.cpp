#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/rollback/InputJournal.h"
#include "game/rollback/RollbackInputs.h"
#include "game/rollback/RollbackSessionHarness.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = MatchSession::Phase;
using Admission = MatchSession::Admission;
constexpr PacketTransport::Connection kHost = 99;

struct Wire final : PacketTransport {
    struct Packet {
        Connection connection = 0;
        std::vector<u8> bytes;
        Delivery delivery = Delivery::Unreliable;
    };
    SendResult result = SendResult::Sent;
    usize budget = std::numeric_limits<usize>::max();
    std::vector<Packet> sent;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        if (result != SendResult::Sent) {
            return result;
        }
        if (budget == 0) {
            return SendResult::Congested;
        }
        --budget;
        sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return std::nullopt;
    }
    void close(Connection /*connection*/) override {}
};

std::vector<u8> inputBytes(std::span<const InputCommand> frame) {
    const auto encoded = InputPacket::encode(frame);
    REQUIRE(encoded);
    return *encoded;
}
std::vector<u8> inputMessage(u8 origin, std::span<const InputCommand> frame, u64 nextTick = 1,
                             u64 serial = 1) {
    REQUIRE_FALSE(frame.empty());
    const RollbackExchange::Message message{frame.front().epoch,          origin, nextTick, {},
                                            {frame.begin(), frame.end()}, serial};
    const auto encoded = RollbackExchange::encode(message);
    REQUIRE(encoded);
    return *encoded;
}
std::vector<u8> controlBytes(MatchControlKind kind, const MatchContext& context) {
    const auto encoded = MatchControlPacket::encode({kind, MatchStop::None, context});
    REQUIRE(encoded);
    return *encoded;
}

struct Room {
    MatchOwners owners;
    std::array<MatchSession, 5> sessions;
    std::array<Wire, 5> wires;
    std::array<test::RollbackSessionHarness, 5> inputs;
    std::array<u64, 5> bound{};
    explicit Room(MatchOwners roster = {1, 2, 0, 0}, u8 delay = 0) : owners(roster) {
        std::vector<MatchLink> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, PacketTransport::Connection{100} + peer});
                const std::array host{MatchLink{1, kHost}};
                REQUIRE(sessions[peer].open(peer, owners, host, delay));
            }
        }
        REQUIRE(sessions[1].open(1, owners, links, delay));
    }
    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
    void deliver(u8 source, MatchSession::Time now = MatchSession::Clock::now()) {
        auto packets = std::move(wires[source].sent);
        wires[source].sent.clear();
        for (const auto& packet : packets) {
            const auto target = source == 1 ? static_cast<u8>(packet.connection - 100) : u8{1};
            const auto sender = source == 1 ? kHost : 100 + source;
            CAPTURE(source, target);
            CHECK(packet.delivery == (RollbackExchange::recognizes(packet.bytes)
                                          ? PacketTransport::Delivery::Unreliable
                                          : PacketTransport::Delivery::Reliable));
            const auto admitted = sessions[target].receive(sender, packet.bytes, now);
            REQUIRE((
                admitted == Admission::Accepted || admitted == Admission::Stale ||
                (sessions[target].phase() == Phase::Stopped && admitted == Admission::WrongPhase)));
        }
    }
    void pump() {
        for (u8 pass = 0; pass < 4; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (present(peer)) {
                    sessions[peer].flush(wires[peer]);
                    deliver(peer);
                }
            }
        }
    }
    bool loaded(u8 peer) {
        if (sessions[peer].context().transition != MatchTransition::Shop &&
            bound[peer] != sessions[peer].context().epoch) {
            if (!inputs[peer].bind(sessions[peer])) {
                return false;
            }
            bound[peer] = sessions[peer].context().epoch;
        }
        return sessions[peer].loaded();
    }
    void prepare() {
        REQUIRE(sessions[1].prepare(7));
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Loading);
                CHECK_FALSE(inputs[peer].advance());
                CHECK_FALSE(inputs[peer].sampleReady());
            }
        }
    }
    void commit() {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
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
    void ready() {
        prepare();
        commit();
    }
    std::vector<InputCommand> commands(u8 peer, u64 tick, bool actions = true) const {
        std::vector<InputCommand> frame;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] != peer) {
                continue;
            }
            InputCommand command;
            command.epoch = sessions[1].context().epoch;
            command.tick = tick;
            command.grant = sessions[1].context().grants[seat];
            command.seat = seat;
            if (actions) {
                command.direction = {1, 0};
                command.magnitude = 0.5f;
                command.aimDirection = Vec3{0, 0, 1};
                command.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
                if (tick % 3 == 0) {
                    command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
                }
            }
            frame.push_back(command);
        }
        return frame;
    }
    void advanceAll(u64 tick, bool actions) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (!present(peer)) {
                continue;
            }
            CAPTURE(peer, tick);
            const auto frame = inputs[peer].advance();
            REQUIRE(frame);
            CHECK(frame->predictedSeats == 0);
            for (u8 source = 1; source <= 4; ++source) {
                if (!present(source)) {
                    continue;
                }
                for (const auto& expected : commands(source, tick, actions)) {
                    const auto& actual = frame->commands[expected.seat];
                    CHECK(inputBytes(std::span{&actual, 1}) == inputBytes(std::span{&expected, 1}));
                }
            }
            CHECK(inputs[peer].tick() == tick + 1);
            REQUIRE(inputs[peer].complete());
        }
    }
    void agreeTerminal(MatchSession::Time now = MatchSession::Clock::now()) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(inputs[peer].submitBoundaryDigest(now));
            }
        }
        for (u8 pass = 0; pass < 4; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (!present(peer)) {
                    continue;
                }
                sessions[peer].flush(wires[peer], now);
                deliver(peer, now);
            }
        }
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(inputs[peer].completeBoundary());
            }
        }
    }
    MatchParty party() const {
        MatchParty result;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                CharacterProfile profile;
                profile.name = "NATIVE";
                profile.character = static_cast<s32>(seat);
                profile.gold = static_cast<s32>(seat) * 100;
                result[seat] = profile;
            }
        }
        return result;
    }
};
} // namespace

TEST_CASE("match control authenticates every supported native startup delay",
          "[match-lockstep][match-rollback]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2}, u8{3}, u8{4});
    STATIC_REQUIRE(RollbackInputs::kMaxDelay == 4);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, delay);
    REQUIRE(room->sessions[1].prepare(7));
    const auto context = room->sessions[1].context();
    REQUIRE(context.inputLead == delay);
    const auto bytes = controlBytes(MatchControlKind::Prepare, context);
    REQUIRE(bytes.size() == MatchControlPacket::kBytes);
    CHECK(bytes[4] == MatchControlPacket::kVersion);
    REQUIRE(MatchControlPacket::decode(bytes));
    CHECK(MatchControlPacket::decode(bytes)->context == context);
    for (usize length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(MatchControlPacket::decode(std::span{bytes}.first(length)));
    }
    auto invalid = bytes;
    invalid.push_back(0);
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    for (const usize offset : {4U, 48U, 49U, 50U, 51U, 52U, 56U, 60U, 63U}) {
        invalid = bytes;
        invalid[offset] = 255;
        CHECK_FALSE(MatchControlPacket::decode(invalid));
    }
    invalid = bytes;
    invalid[4] = 6; // prior lockstep protocol is not compatible
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    for (const u8 unsupported :
         {static_cast<u8>(RollbackInputs::kMaxDelay + 1), std::numeric_limits<u8>::max()}) {
        invalid = bytes;
        invalid[22] = unsupported;
        CHECK_FALSE(MatchControlPacket::decode(invalid));
        auto invalidContext = context;
        invalidContext.inputLead = unsupported;
        CHECK_FALSE(MatchControlPacket::encode(
            {MatchControlKind::Prepare, MatchStop::None, invalidContext}));
    }
}

TEST_CASE("native match delay must match the authenticated host prepare",
          "[match-lockstep][match-rollback]") {
    const u8 offeredDelay = GENERATE(u8{0}, u8{1}, u8{2}, u8{3}, u8{4});
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, offeredDelay);
    REQUIRE(room->sessions[1].prepare(7));
    const auto offered = room->sessions[1].context();
    const std::array hostLink{MatchLink{1, kHost}};
    for (u8 delay = 0; delay <= RollbackInputs::kMaxDelay; ++delay) {
        if (delay == offeredDelay) {
            continue;
        }
        MatchSession wrong;
        REQUIRE(wrong.open(2, room->owners, hostLink, delay));
        CHECK(wrong.receive(kHost, controlBytes(MatchControlKind::Prepare, offered)) ==
              Admission::Invalid);
        CHECK(wrong.phase() == Phase::Lobby);
    }
    for (const u8 unsupported :
         {static_cast<u8>(RollbackInputs::kMaxDelay + 1), std::numeric_limits<u8>::max()}) {
        MatchSession invalid;
        CHECK_FALSE(invalid.open(2, room->owners, hostLink, unsupported));
    }
    CHECK(room->sessions[2].receive(kHost, controlBytes(MatchControlKind::Prepare, offered)) ==
          Admission::Accepted);
}

TEST_CASE("native match advances identical complete frames on every peer without snapshots",
          "[match-lockstep][match-rollback]") {
    const auto owners = GENERATE(MatchOwners{1, 2, 0, 0}, MatchOwners{1, 2, 3, 0},
                                 MatchOwners{1, 2, 3, 4}, MatchOwners{2, 1, 2, 3});
    const auto delay = GENERATE(u8{0}, u8{1}, u8{2}, u8{3}, u8{4});
    auto room = std::make_unique<Room>(owners, delay);
    room->ready();
    for (u64 tick = 1; tick <= 20 + delay; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                CHECK(room->inputs[peer].sampleTick() == tick + delay);
                REQUIRE(room->inputs[peer].sample(room->commands(peer, tick + delay)));
            }
        }
        room->pump();
        room->advanceAll(tick, tick > delay);
    }
}

TEST_CASE("native match holds authenticated inputs arriving before the local commit poll",
          "[match-lockstep][match-rollback]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->prepare();
    for (u8 const peer : {u8{1}, u8{2}, u8{3}}) {
        REQUIRE(room->loaded(peer));
    }
    room->sessions[2].flush(room->wires[2]);
    room->deliver(2);
    room->sessions[3].flush(room->wires[3]);
    room->deliver(3);
    REQUIRE(room->sessions[1].phase() == Phase::Running);
    room->sessions[1].flush(room->wires[1]);
    std::vector<u8> lateCommit;
    for (const auto& packet : room->wires[1].sent) {
        const auto control = MatchControlPacket::decode(packet.bytes);
        if (packet.connection == 103 && control && control->kind == MatchControlKind::Commit) {
            lateCommit = packet.bytes;
        } else {
            const auto target = static_cast<u8>(packet.connection - 100);
            REQUIRE(room->sessions[target].receive(kHost, packet.bytes) == Admission::Accepted);
        }
    }
    room->wires[1].sent.clear();
    REQUIRE_FALSE(lateCommit.empty());
    REQUIRE(room->inputs[2].sample(room->commands(2, 1)));
    room->sessions[2].flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(room->inputs[1].sample(room->commands(1, 1)));
    room->sessions[1].flush(room->wires[1]);
    room->deliver(1);
    REQUIRE(room->sessions[3].phase() == Phase::Loading);
    CHECK_FALSE(room->inputs[3].advance());
    CHECK_FALSE(room->inputs[3].sample(room->commands(3, 1)));
    REQUIRE(room->sessions[3].receive(kHost, lateCommit) == Admission::Accepted);
    REQUIRE(room->inputs[3].sample(room->commands(3, 1)));
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("native match sends commit before input and preserves both through congestion",
          "[match-lockstep][match-rollback]") {
    auto room = std::make_unique<Room>();
    room->prepare();
    REQUIRE(room->loaded(1));
    REQUIRE(room->loaded(2));
    room->sessions[2].flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(room->sessions[1].phase() == Phase::Running);
    REQUIRE(room->inputs[1].sample(room->commands(1, 1)));
    room->wires[1].result = PacketTransport::SendResult::Congested;
    room->sessions[1].flush(room->wires[1]);
    CHECK(room->wires[1].sent.empty());
    CHECK(room->sessions[1].queuedControls() == 1);
    room->wires[1].result = PacketTransport::SendResult::Sent;
    room->wires[1].budget = 1;
    room->sessions[1].flush(room->wires[1]);
    REQUIRE(room->wires[1].sent.size() == 1);
    REQUIRE(MatchControlPacket::decode(room->wires[1].sent[0].bytes));
    CHECK(MatchControlPacket::decode(room->wires[1].sent[0].bytes)->kind ==
          MatchControlKind::Commit);
    room->deliver(1);
    REQUIRE(room->sessions[2].phase() == Phase::Running);
    room->wires[1].budget = std::numeric_limits<usize>::max();
    room->sessions[1].flush(room->wires[1]);
    room->deliver(1);
    REQUIRE(room->inputs[2].sample(room->commands(2, 1)));
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("native prediction is bounded and never fabricates local device samples",
          "[match-lockstep][match-rollback]") {
    auto room = std::make_unique<Room>();
    room->ready();
    room->wires[2].result = PacketTransport::SendResult::Congested;
    for (u64 tick = 1; tick <= RollbackInputs::kPredictionTicks; ++tick) {
        REQUIRE(room->inputs[1].sample(room->commands(1, tick)));
        const auto frame = room->inputs[1].advance();
        REQUIRE(frame);
        CHECK(frame->predictedSeats == 2);
        REQUIRE(room->inputs[1].complete());
        CHECK(room->sessions[1].nativeConfirmed() == 0);
    }
    REQUIRE(room->inputs[1].sample(room->commands(1, 9)));
    CHECK_FALSE(room->inputs[1].advance());
    CHECK_FALSE(room->inputs[1].sample(room->commands(1, 10)));
    room->sessions[1].update(60);
    CHECK(room->inputs[1].tick() == 9);
    CHECK_FALSE(room->sessions[1].confirmNativeTick(room->sessions[1].context().epoch, 1));
    // Missing actual inputs correct the retained suffix, not an invented held edge.
    std::vector<InputCommand> delayed;
    for (u64 tick = 1; tick <= 8; ++tick) {
        const auto frame = room->commands(2, tick, false);
        delayed.insert(delayed.end(), frame.begin(), frame.end());
    }
    REQUIRE(room->sessions[1].receive(102, inputMessage(2, delayed, 9, 2)) == Admission::Accepted);
    REQUIRE(room->inputs[1].advance());
    REQUIRE(room->inputs[1].complete());
    CHECK(room->sessions[1].nativeConfirmed() == 8);
}

TEST_CASE("native sessions recover retained inputs after an outage beyond the prediction window",
          "[match-rollback][session-recovery]") {
    const auto roster =
        GENERATE(MatchOwners{1, 2, 0, 0}, MatchOwners{1, 1, 1, 2}, MatchOwners{1, 2, 3, 4});
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    const bool congested = GENERATE(false, true);
    CAPTURE(roster, delay, congested);
    auto room = std::make_unique<Room>(roster, delay);
    room->ready();
    std::map<u64, RollbackInputs::Commands> actual;
    const auto sample = [&](u8 peer, bool outage) {
        auto& inputs = room->inputs[peer];
        if (!inputs.sampleReady()) {
            return;
        }
        auto commands = room->commands(peer, inputs.sampleTick());
        if (outage) {
            for (auto& command : commands) {
                // The corrected branch must differ from held remote prediction,
                // including an edge which cannot be recovered by holding a button.
                command.magnitude = 0.75f;
                command.pressedButtons = static_cast<u32>(CommandPress::Attack);
            }
        }
        for (const auto& command : commands) {
            actual[command.tick][command.seat] = command;
        }
        REQUIRE(inputs.sample(commands));
    };
    for (u64 tick = 1; tick <= 6; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                sample(peer, false);
            }
        }
        room->pump();
        room->advanceAll(tick, tick > delay);
        room->pump();
    }
    const auto outageAt = MatchSession::Clock::now();
    std::array<u64, 5> stalledHead{};
    std::array<u32, 5> waits{};
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (room->present(peer) && congested) {
            room->wires[peer].result = PacketTransport::SendResult::Congested;
        }
    }
    // One virtual second, not a sleep or a claimed real-network latency test.
    // Sent datagrams are discarded in the loss variant; the congestion variant
    // refuses sends. Both must recover through the same normal retransmission path.
    for (u32 turn = 0; turn < 60; ++turn) {
        const auto now = outageAt + std::chrono::milliseconds{turn * 17};
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (!room->present(peer)) {
                continue;
            }
            sample(peer, true);
            auto& inputs = room->inputs[peer];
            auto& session = room->sessions[peer];
            if (!inputs.advance()) {
                if (stalledHead[peer] == 0) {
                    stalledHead[peer] = inputs.tick();
                }
                CHECK(inputs.tick() == stalledHead[peer]);
                ++waits[peer];
            }
            REQUIRE(inputs.complete());
            CHECK(inputs.tick() - 1 - inputs.confirmedTick() <= RollbackInputs::kPredictionTicks);
            CHECK(session.nativeConfirmed() <= 6 + delay);
            CHECK_FALSE(session.completeNativeBoundary(session.context().epoch, inputs.tick() - 1));
            session.update(1.0 / 60, now);
            session.flush(room->wires[peer], now);
            for (const auto& packet : room->wires[peer].sent) {
                CHECK(RollbackExchange::recognizes(packet.bytes));
            }
            room->wires[peer].sent.clear();
            REQUIRE(session.phase() == Phase::Running);
        }
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (!room->present(peer)) {
            continue;
        }
        CHECK(waits[peer] > RollbackInputs::kPredictionTicks);
        room->wires[peer].result = PacketTransport::SendResult::Sent;
    }
    room->pump(); // Only retained send history/receiver ACKs repair the gap.
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (!room->present(peer)) {
            continue;
        }
        auto corrected = room->inputs[peer].inputs();
        const auto head = corrected.tick();
        REQUIRE(corrected.mismatchTick());
        REQUIRE(corrected.rewind(*corrected.mismatchTick()));
        while (corrected.tick() < head) {
            const auto tick = corrected.tick();
            const auto frame = corrected.advance();
            REQUIRE(frame);
            CHECK(frame->predictedSeats == 0);
            REQUIRE(actual.contains(tick));
            // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
            for (u8 seat = 0; seat < static_cast<u8>(roster.size()); ++seat) {
                if (roster[seat] == 0) {
                    continue;
                }
                CHECK(inputBytes(std::span{&frame->commands[seat], 1}) ==
                      inputBytes(std::span{&actual.at(tick)[seat], 1}));
            }
        }
        REQUIRE(room->inputs[peer].advance());
        REQUIRE(room->inputs[peer].complete());
        CHECK(room->sessions[peer].nativeConfirmed() == room->inputs[peer].tick() - 1);
    }
    room->pump();
    // Recovery is not merely one unblocked tick: keep accepting fresh devices,
    // confirming all seats and progressing the ordinary session afterward.
    for (u32 turn = 0; turn < 12; ++turn) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                sample(peer, false);
            }
        }
        room->pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (!room->present(peer)) {
                continue;
            }
            REQUIRE(room->inputs[peer].advance());
            REQUIRE(room->inputs[peer].complete());
            CHECK(room->sessions[peer].nativeConfirmed() == room->inputs[peer].tick() - 1);
            CHECK(room->inputs[peer].tick() == room->inputs[1].tick());
        }
        room->pump();
    }
}

TEST_CASE("disconnect discards speculative terminal authority and a fresh binding starts cleanly",
          "[match-rollback][session-recovery]") {
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    const auto oldEpoch = host.context().epoch;
    const auto delayedInput = inputMessage(2, room->commands(2, 1));
    const auto delayedReady = controlBytes(MatchControlKind::Ready, host.context());
    REQUIRE(room->inputs[1].sample(room->commands(1, 1)));
    REQUIRE(room->inputs[1].advance());
    REQUIRE(room->inputs[1].complete());
    CHECK(host.nativeConfirmed() == 0);
    CHECK_FALSE(host.observeNativeTerminal(oldEpoch, 1, {}));
    CHECK_FALSE(host.completeNativeBoundary(oldEpoch, 1));
    // The lost guest is dropped and the host plays on; the speculative terminal it
    // never agreed is no authority, and leaving afterwards reads as leaving.
    REQUIRE(host.disconnected(102));
    CHECK(host.phase() == Phase::Running);
    CHECK(host.dropped(2));
    CHECK(host.failureDetail().empty());
    CHECK_FALSE(host.nativeTerminalAgreed(oldEpoch, 1));
    host.leave();
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::Left);
    CHECK_FALSE(host.failureDetail().empty());
    CHECK_FALSE(room->inputs[1].advance());
    CHECK_FALSE(room->inputs[1].publish());
    const auto reason = host.failureDetail();
    host.update(60);
    CHECK(host.failureDetail() == reason);

    // clear() is not in-place reconnection. The documented fresh transport/room
    // lifetime has different authenticated connections, even if epoch/seat IDs repeat.
    room->inputs[1].clear();
    host.clear();
    CHECK(host.failureDetail().empty());
    CHECK(host.nativeHead() == 0);
    CHECK(host.nativeConfirmed() == 0);
    CHECK_FALSE(host.nativeTerminalAgreed(oldEpoch, 1));
    const std::array links{MatchLink{2, 902}};
    REQUIRE(host.open(1, {1, 2, 0, 0}, links, 2));
    REQUIRE(host.prepare(7));
    test::RollbackSessionHarness fresh;
    REQUIRE(fresh.bind(host));
    REQUIRE(host.loaded());
    REQUIRE(host.receive(902, controlBytes(MatchControlKind::Ready, host.context())) ==
            Admission::Accepted);
    REQUIRE(host.phase() == Phase::Running);
    CHECK(host.receive(102, delayedInput) == Admission::WrongPeer);
    CHECK(host.receive(102, delayedReady) == Admission::WrongPeer);
    CHECK(fresh.tick() == 1);
    CHECK(fresh.sampleTick() == 3);
    const auto first = fresh.advance();
    REQUIRE(first);
    CHECK(first->predictedSeats == 0);
    CHECK(first->commands[0].pressedButtons == 0);
    CHECK(first->commands[1].pressedButtons == 0);
    REQUIRE(fresh.complete());
    CHECK(host.nativeConfirmed() == 1);
    CHECK(host.failureDetail().empty());
    fresh.clear();
}

TEST_CASE("native match refuses global pause resume and replicated gameplay streams",
          "[match-lockstep][match-rollback]") {
    auto room = std::make_unique<Room>();
    room->ready();
    for (u8 peer = 1; peer <= 2; ++peer) {
        CHECK_FALSE(room->sessions[peer].requestPause());
        CHECK(room->sessions[peer].phase() == Phase::Running);
    }
    CHECK_FALSE(room->sessions[1].prepare(7, MatchTransition::Resume));
    auto context = room->sessions[1].context();
    CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::Pause, MatchStop::None, context}));
    CHECK_FALSE(
        MatchControlPacket::encode({MatchControlKind::RequestPause, MatchStop::None, context}));
    context.transition = MatchTransition::Resume;
    CHECK_FALSE(context.valid());
    for (const std::array<u8, 4> obsolete :
         {std::array<u8, 4>{'G', 'D', 'L', 'F'}, std::array<u8, 4>{'G', 'D', 'L', 'A'},
          std::array<u8, 4>{'G', 'D', 'L', 'C'}}) {
        CHECK(room->sessions[1].receive(102, obsolete) == Admission::Invalid);
        CHECK(room->sessions[2].receive(kHost, obsolete) == Admission::Invalid);
    }
    CHECK(room->sessions[1].receive(102, inputBytes(room->commands(2, 1))) == Admission::Invalid);
}

TEST_CASE("native match conflict consumer failure and disconnect end the epoch",
          "[match-lockstep][match-rollback]") {
    const u32 failure = GENERATE(0U, 1U, 2U, 3U);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    if (failure == 1) {
        room->prepare();
        REQUIRE(room->sessions[1].attachNativeInputs(
            room->sessions[1].context().epoch,
            [](const RollbackExchange::Message&, std::optional<u8>) { return false; }));
        REQUIRE(room->sessions[1].loaded());
        for (u8 const peer : {u8{2}, u8{3}, u8{4}}) {
            REQUIRE(room->loaded(peer));
        }
        // Ready controls are admitted before any input heartbeat reaches the
        // rejecting consumer, giving it a real active-epoch admission failure.
        for (u8 const peer : {u8{2}, u8{3}, u8{4}}) {
            REQUIRE(
                room->sessions[1].receive(100 + peer, controlBytes(MatchControlKind::Ready,
                                                                   room->sessions[1].context())) ==
                Admission::Accepted);
        }
    } else {
        room->ready();
    }
    auto& host = room->sessions[1];
    if (failure == 0) {
        const auto original = room->commands(2, 1, false);
        REQUIRE(host.receive(102, inputMessage(2, original)) == Admission::Accepted);
        auto changed = original;
        changed.front().pressedButtons = static_cast<u32>(CommandPress::Attack);
        CHECK(host.receive(102, inputMessage(2, changed)) == Admission::Invalid);
    } else if (failure == 1) {
        CHECK(host.receive(102, inputMessage(2, room->commands(2, 1))) == Admission::Invalid);
    } else if (failure == 2) {
        REQUIRE(host.disconnected(102));
        CHECK(host.phase() == Phase::Running);
        CHECK(host.dropped(2));
        return; // the host drops that guest and plays on; nothing stops here
    } else {
        REQUIRE(room->inputs[1].sample(room->commands(1, 1)));
        room->wires[1].result = PacketTransport::SendResult::Invalid;
        host.flush(room->wires[1]);
    }
    REQUIRE(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::TransportFailure);
    CHECK_FALSE(room->inputs[1].advance());
    CHECK_FALSE(room->inputs[1].sample(room->commands(1, 2)));
}

TEST_CASE("default match opening uses one buffered tick on every peer",
          "[match-lockstep][match-rollback]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, MatchContext{}.inputLead);
    room->ready();
    CHECK(room->sessions[1].inputLead() == 1);
    CHECK(room->sessions[2].inputLead() == 1);
    room->advanceAll(1, false);
    CHECK_FALSE(room->inputs[1].advance());
    CHECK_FALSE(room->inputs[2].advance());
}

TEST_CASE("native match distinguishes received input speculative head and confirmed terminal",
          "[match-lockstep][match-rollback][native-travel]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 2);
    room->ready();
    auto& host = room->sessions[1];
    const auto party = room->party();
    const auto epoch = host.context().epoch;
    CHECK_FALSE(host.confirmNativeTick(epoch, 1));
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party));
    REQUIRE(room->inputs[1].advance());
    REQUIRE(room->inputs[1].publish());
    CHECK(host.nativeHead() == 2);
    CHECK_FALSE(host.completeNativeBoundary(epoch, 1));
    REQUIRE(room->inputs[1].complete());
    CHECK(host.nativeConfirmed() == 1);
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK_FALSE(host.completeNativeBoundary(epoch, 1));
    REQUIRE(room->inputs[2].advance());
    REQUIRE(room->inputs[2].complete());
    room->agreeTerminal();
    REQUIRE(host.completeNativeBoundary(epoch, 1));
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.context().sourceEpoch == epoch);
    CHECK(host.context().sourceTick == 1);
}

TEST_CASE("native travel drains exact source inputs before profiles shop and new epoch",
          "[match-lockstep][match-rollback][native-travel]") {
    const auto transition = GENERATE(MatchTransition::Travel, MatchTransition::SecretEnter,
                                     MatchTransition::SecretReturn, MatchTransition::Shop);
    const u32 progress = GENERATE(0U, 1U, 2U);
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const auto source = host.context();
    const auto local = room->commands(1, 1);
    const auto remote = room->commands(2, 1);
    REQUIRE(room->inputs[1].sample(local));
    REQUIRE(room->inputs[2].sample(remote));
    guest.flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(room->inputs[1].advance());
    REQUIRE(room->inputs[1].submitBoundaryDigest());
    CHECK_FALSE(room->inputs[1].completeBoundary());
    if (progress != 0) {
        host.flush(room->wires[1]);
        room->deliver(1);
        REQUIRE(room->inputs[2].advance());
        if (progress == 2) {
            REQUIRE(room->inputs[2].complete());
        }
    }
    if (progress == 0) {
        host.flush(room->wires[1]);
        room->deliver(1);
        REQUIRE(room->inputs[2].advance());
    }
    room->agreeTerminal();
    const auto profiles = room->party();
    ShopSetup shop;
    shop.source = 8;
    shop.destination = 9;
    REQUIRE(host.prepare(8, transition, &profiles,
                         transition == MatchTransition::Shop ? &shop : nullptr,
                         transition == MatchTransition::SecretReturn));
    const auto next = host.context();
    CHECK(next.sourceEpoch == source.epoch);
    CHECK(next.sourceTick == 1);
    if (transition == MatchTransition::Travel) {
        REQUIRE(host.requestMovieSkip(0));
    }
    host.flush(room->wires[1]);
    std::vector<Wire::Packet> delayed;
    for (const auto& packet : room->wires[1].sent) {
        if (RollbackExchange::recognizes(packet.bytes)) {
            delayed.push_back(packet);
        } else {
            REQUIRE(guest.receive(kHost, packet.bytes) == Admission::Accepted);
        }
    }
    room->wires[1].sent.clear();
    REQUIRE(guest.nativeDraining());
    CHECK(guest.context() == source);
    CHECK(guest.travelParty() == nullptr);
    CHECK_FALSE(guest.loaded());
    CHECK_FALSE(guest.confirmNativeTick(source.epoch, 2));
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, next)) ==
            Admission::Stale);
    CHECK(host.receive(102, inputMessage(2, remote)) == Admission::Accepted);
    for (const auto& packet : delayed) {
        REQUIRE(guest.receive(kHost, packet.bytes) == Admission::Accepted);
    }
    REQUIRE(room->inputs[2].completeBoundary());
    CHECK_FALSE(guest.nativeDraining());
    CHECK(guest.context() == next);
    CHECK(guest.phase() == Phase::Loading);
    REQUIRE(guest.travelParty());
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        if (profiles[seat]) {
            CHECK(CharacterProfilePacket::encode(*(*guest.travelParty())[seat]) ==
                  CharacterProfilePacket::encode(*profiles[seat]));
        }
    }
    if (transition == MatchTransition::Shop) {
        REQUIRE(guest.shop().setup());
    }
    if (transition == MatchTransition::Travel) {
        CHECK(guest.movieSkipVotes() == 1);
        REQUIRE(guest.requestMovieSkip(1));
        room->pump();
        CHECK(host.movieSkipReady());
        CHECK(guest.movieSkipReady());
    }
    room->commit();
    CHECK(guest.receive(kHost, inputMessage(1, local)) == Admission::Stale);
    CHECK(host.receive(102, inputMessage(2, remote)) == Admission::Stale);
    if (transition == MatchTransition::Shop) {
        CHECK(host.nativeHead() == 0);
        CHECK(guest.nativeHead() == 0);
        REQUIRE(host.shop().finish());
        REQUIRE(guest.shop().finish());
        room->pump();
        REQUIRE(host.shop().finished());
        REQUIRE(host.prepare(9, MatchTransition::Travel, &profiles));
        CHECK(host.context().sourceTick == 0);
        room->pump();
        room->commit();
    }
    CHECK(host.nativeHead() == 1);
    CHECK(guest.nativeHead() == 1);
    REQUIRE(room->inputs[1].sample(room->commands(1, 1)));
    REQUIRE(room->inputs[2].sample(room->commands(2, 1)));
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("terminal agreement waits for missing earlier inputs despite a later source command",
          "[match-rollback][native-travel]") {
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const auto sourceEpoch = host.context().epoch;
    std::vector<InputCommand> hostCommands;
    for (u64 tick = 1; tick <= 3; ++tick) {
        const auto local = room->commands(1, tick);
        hostCommands.insert(hostCommands.end(), local.begin(), local.end());
        REQUIRE(room->inputs[1].sample(local));
        REQUIRE(room->inputs[2].sample(room->commands(2, tick)));
        guest.flush(room->wires[2]);
        room->deliver(2);
        REQUIRE(room->inputs[1].advance());
        REQUIRE(room->inputs[1].complete());
        REQUIRE(room->inputs[2].advance());
        REQUIRE(room->inputs[2].complete());
    }
    CHECK(guest.nativeConfirmed() == 0);
    REQUIRE(guest.receive(kHost, inputMessage(1, std::span{hostCommands}.last(1), 4, 999)) ==
            Admission::Accepted);
    REQUIRE(room->inputs[1].submitBoundaryDigest());
    CHECK_FALSE(host.completeNativeBoundary(sourceEpoch, 3));
    const auto party = room->party();
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party));
    // The source scene/consumer remains bound while the transport recovers its
    // missing prefix, even though the latest command already advertised its end.
    room->pump();
    CHECK_FALSE(room->inputs[2].advance()); // correction only; no next local sample
    REQUIRE(room->inputs[2].complete());
    CHECK(guest.nativeConfirmed() == 3);
    room->agreeTerminal();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    room->pump();
    REQUIRE(room->inputs[2].completeBoundary());
    CHECK(guest.context() == host.context());
    room->commit();
}

TEST_CASE("native transition fences refuse stale impossible and conflicting boundaries",
          "[match-lockstep][match-rollback][native-travel]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 2);
    room->ready();
    room->advanceAll(1, false);
    auto& guest = room->sessions[2];
    const auto old = guest.context();
    auto next = old;
    ++next.epoch;
    next.transition = MatchTransition::Travel;
    next.scene = 8;
    next.sourceEpoch = old.epoch;
    next.sourceTick = 2;
    REQUIRE(next.valid());
    auto changed = next;
    changed.sourceEpoch = 0;
    CHECK_FALSE(changed.valid());
    changed = next;
    ++changed.sourceEpoch;
    CHECK_FALSE(changed.valid());
    changed = next;
    changed.sourceTick = 0;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    changed = next;
    changed.sourceTick = RollbackInputs::kMaxAhead + 2;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, next)) ==
            Admission::Accepted);
    changed = next;
    ++changed.sourceTick;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    auto profiles = room->party();
    auto checkpoint = PartyCheckpointPacket::encode({next.epoch, 0, *profiles[0]});
    REQUIRE(checkpoint);
    REQUIRE(guest.receive(kHost, *checkpoint) == Admission::Accepted);
    REQUIRE(guest.receive(kHost, *checkpoint) == Admission::Accepted);
    ++profiles[0]->gold;
    checkpoint = PartyCheckpointPacket::encode({next.epoch, 0, *profiles[0]});
    REQUIRE(checkpoint);
    CHECK(guest.receive(kHost, *checkpoint) == Admission::Invalid);
    CHECK(guest.context() == old);
    CHECK(guest.travelParty() == nullptr);
    guest.update(MatchSession::kLoadTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::LoadTimeout);
    CHECK_FALSE(room->inputs[2].advance());
}

namespace {
NativeAgreement::Digest observed(u8 value = 0) {
    NativeAgreement::Digest digest{};
    digest[0] = value;
    digest.back() = static_cast<u8>(value ^ 0xa5);
    return digest;
}
std::vector<u8> observationReport(u64 epoch, const NativeAgreement::Digest& digest) {
    NativeAgreement sender;
    const std::array links{NativeAgreement::Link{1, kHost}};
    REQUIRE(sender.open(2, epoch, {1, 2, 0, 0}, links, MatchSession::kNativeAgreementPeriod,
                        MatchSession::Time{}));
    REQUIRE(sender.submit(MatchSession::kNativeAgreementPeriod, digest, MatchSession::Time{}) ==
            NativeAgreement::Admission::Accepted);
    Wire wire;
    REQUIRE(sender.flush(wire, MatchSession::Time{}));
    REQUIRE(wire.sent.size() == 1);
    return wire.sent.front().bytes;
}
void pumpAgreement(Room& room, MatchSession::Time now = {}) {
    for (u8 pass = 0; pass < 3; ++pass) {
        for (u8 source = 1; source <= 4; ++source) {
            if (!room.present(source)) {
                continue;
            }
            room.sessions[source].flush(room.wires[source], now);
            auto packets = std::move(room.wires[source].sent);
            room.wires[source].sent.clear();
            for (const auto& packet : packets) {
                CHECK(packet.delivery == (RollbackExchange::recognizes(packet.bytes)
                                              ? PacketTransport::Delivery::Unreliable
                                              : PacketTransport::Delivery::Reliable));
                const auto target = source == 1 ? static_cast<u8>(packet.connection - 100) : u8{1};
                const auto connection = source == 1 ? kHost : 100 + source;
                if (room.sessions[target].dropped(source)) {
                    continue; // that link is closed; its late traffic never arrives
                }
                const bool stopped = room.sessions[target].phase() == Phase::Stopped;
                const auto result = room.sessions[target].receive(connection, packet.bytes, now);
                CAPTURE(source, target, static_cast<u32>(result),
                        RollbackExchange::recognizes(packet.bytes),
                        NativeAgreement::recognizes(packet.bytes), packet.bytes.size());
                if (stopped) {
                    // Reliable traffic already in flight can precede the Stop
                    // echo (notably a peer's scheduled observation at timeout).
                    if (RollbackExchange::recognizes(packet.bytes)) {
                        REQUIRE(RollbackExchange::decode(packet.bytes));
                    } else if (NativeAgreement::recognizes(packet.bytes)) {
                        REQUIRE(packet.bytes.size() == NativeAgreement::kPacketBytes);
                    } else {
                        const auto control = MatchControlPacket::decode(packet.bytes);
                        REQUIRE(control);
                        CHECK(control->kind == MatchControlKind::Stop);
                    }
                    // A disconnected authenticated link is removed before its
                    // already-queued traffic can arrive. Neither refusal may
                    // revive the stopped session.
                    CHECK((result == Admission::WrongPhase || result == Admission::WrongPeer));
                } else {
                    REQUIRE((result == Admission::Accepted || result == Admission::Stale));
                }
            }
        }
    }
}
void executeThrough(Room& room, u64 last) {
    while (room.inputs[1].tick() <= last) {
        const auto tick = room.inputs[1].tick();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room.present(peer)) {
                REQUIRE(room.inputs[peer].sample(
                    room.commands(peer, room.inputs[peer].sampleTick(), false)));
            }
        }
        pumpAgreement(room);
        room.advanceAll(tick, false);
    }
}
} // namespace

TEST_CASE("off-cadence terminal certificates reject disagreement and gate transition",
          "[match-rollback][native-travel][match-native-agreement]") {
    const bool mismatch = GENERATE(false, true);
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 31);
    for (u8 const peer : {u8{1}, u8{2}}) {
        REQUIRE(room->sessions[peer].observeNative(30, observed(), MatchSession::Time{}));
    }
    pumpAgreement(*room);
    const auto epoch = room->sessions[1].context().epoch;
    const auto profiles = room->party();
    REQUIRE(room->sessions[1].observeNativeTerminal(epoch, 31, observed(), MatchSession::Time{}));
    CHECK_FALSE(room->sessions[1].completeNativeBoundary(epoch, 31));
    CHECK_FALSE(room->sessions[1].prepare(8, MatchTransition::Travel, &profiles));
    REQUIRE(room->sessions[2].observeNativeTerminal(epoch, 31, observed(mismatch ? 1 : 0),
                                                    MatchSession::Time{}));
    room->wires[2].result = PacketTransport::SendResult::Congested;
    pumpAgreement(*room);
    CHECK_FALSE(room->sessions[1].nativeTerminalAgreed(epoch, 31));
    CHECK_FALSE(room->sessions[2].nativeTerminalAgreed(epoch, 31));
    CHECK(room->sessions[1].context().epoch == epoch);
    room->wires[2].result = PacketTransport::SendResult::Sent;
    pumpAgreement(*room);
    if (mismatch) {
        CHECK(room->sessions[1].phase() == Phase::Stopped);
        CHECK(room->sessions[2].phase() == Phase::Stopped);
        CHECK(room->sessions[1].stopReason() == MatchStop::TransportFailure);
    } else {
        REQUIRE(room->sessions[1].nativeTerminalAgreed(epoch, 31));
        REQUIRE(room->sessions[2].nativeTerminalAgreed(epoch, 31));
        REQUIRE(room->inputs[1].completeBoundary());
        REQUIRE(room->inputs[2].completeBoundary());
        REQUIRE(room->sessions[1].prepare(8, MatchTransition::Travel, &profiles));
        pumpAgreement(*room);
        REQUIRE(room->inputs[2].completeBoundary());
        room->commit();
        CHECK(room->sessions[1].context().epoch == epoch + 1);
    }
}

TEST_CASE("native match observations are lazy and do not bypass startup barriers",
          "[match-lockstep][match-native-agreement]") {
    SECTION("only Commit opens observation receipt and actual completion opens submission") {
        auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 2);
        for (u8 peer = 1; peer <= 2; ++peer) {
            CHECK_FALSE(room->sessions[peer].observeNative(30, observed(), MatchSession::Time{}));
        }
        room->prepare();
        const auto report = observationReport(room->sessions[1].context().epoch, observed());
        CHECK(room->sessions[1].receive(102, report, MatchSession::Time{}) == Admission::Stale);
        CHECK(room->sessions[1].nativeAgreement() == nullptr);
        room->commit();
        for (u8 peer = 1; peer <= 2; ++peer) {
            auto& session = room->sessions[peer];
            CHECK_FALSE(session.observeNative(0, observed(), MatchSession::Time{}));
            CHECK_FALSE(session.observeNative(30, observed(), MatchSession::Time{}));
            REQUIRE(room->inputs[peer].advance());
            CHECK_FALSE(session.observeNative(30, observed(), MatchSession::Time{}));
            REQUIRE(room->inputs[peer].complete());
            CHECK_FALSE(session.observeNative(1, observed(), MatchSession::Time{}));
            CHECK(session.nativeAgreement() == nullptr);
            session.update(0, MatchSession::Time::max());
            CHECK(session.phase() == Phase::Running); // no idle observer timeout
        }
    }
}

TEST_CASE("terminal epoch-wide failures stop current and draining epochs but not retired ones",
          "[match-rollback][native-travel][match-native-agreement]") {
    const auto phase = GENERATE(0, 1, 2, 3);
    auto room = std::make_unique<Room>();
    room->ready();
    const auto sourceEpoch = room->sessions[1].context().epoch;

    // Obtain a canonical tick-zero Failed packet through the real producer.
    NativeAgreement failed;
    const std::array links{NativeAgreement::Link{1, kHost}};
    REQUIRE(failed.openTerminal(2, sourceEpoch, room->owners, links, 1));
    const std::array<u8, 1> malformed{};
    REQUIRE(failed.receive(kHost, malformed) == NativeAgreement::Admission::Invalid);
    Wire wire;
    CHECK_FALSE(failed.flush(wire));
    REQUIRE(wire.sent.size() == 1);
    const auto bytes = wire.sent.front().bytes;
    REQUIRE(NativeAgreement::terminalContext(bytes));
    REQUIRE(NativeAgreement::terminalContext(bytes)->second == 0);

    if (phase != 0) {
        executeThrough(*room, 1);
        if (phase == 1) {
            for (u8 const peer : {u8{1}, u8{2}}) {
                REQUIRE(room->inputs[peer].submitBoundaryDigest());
            }
        } else {
            room->agreeTerminal();
            const auto profiles = room->party();
            REQUIRE(room->sessions[1].prepare(8, MatchTransition::Travel, &profiles));
            room->pump();
            REQUIRE(room->inputs[2].completeBoundary());
            if (phase == 3) {
                room->commit();
            }
        }
    }
    for (u8 const peer : {u8{1}, u8{2}}) {
        CAPTURE(phase, peer);
        auto& session = room->sessions[peer];
        const auto result = session.receive(peer == 1 ? 102 : kHost, bytes);
        if (phase == 3) {
            CHECK(result == Admission::Stale);
            CHECK(session.phase() == Phase::Running);
            CHECK(session.context().epoch == sourceEpoch + 1);
            CHECK(session.nativeConfirmed() == 0);
        } else {
            CHECK(result == Admission::Accepted);
            CHECK(session.phase() == Phase::Stopped);
            CHECK(session.stopReason() == MatchStop::TransportFailure);
        }
    }
    if (phase == 3) {
        executeThrough(*room, 1);
    }
}

TEST_CASE("native match observations agree without snapshots or changing input progression",
          "[match-lockstep][match-native-agreement]") {
    const auto roster =
        GENERATE((MatchOwners{1, 2, 0, 0}), (MatchOwners{1, 2, 3, 4}), (MatchOwners{2, 1, 2, 4}));
    auto room = std::make_unique<Room>(roster);
    room->ready();
    for (const u64 tick : {u64{30}, u64{60}}) {
        executeThrough(*room, tick);
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                auto& session = room->sessions[peer];
                CHECK_FALSE(session.observeNative(tick - 1, observed(), MatchSession::Time{}));
                REQUIRE(session.observeNative(tick, observed(), MatchSession::Time{}));
                REQUIRE(session.observeNative(tick, observed(), MatchSession::Time{}));
                CHECK(room->inputs[peer].tick() == tick + 1);
                CHECK(room->inputs[peer].sampleTick() == tick + 1);
            }
        }
        pumpAgreement(*room);
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                REQUIRE(room->sessions[peer].nativeAgreement());
                CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == tick);
                CHECK(room->sessions[peer].phase() == Phase::Running);
            }
        }
    }
}

TEST_CASE("the host sees which peer went quiet and drops it so the rest carry on",
          "[match-lockstep][drop]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 10);
    CHECK(room->sessions[1].quietPeers().empty());
    // Peer three stops sampling; the others predict it through the window, then wait.
    const auto stepPair = [&] {
        for (const u8 peer : {u8{1}, u8{2}}) {
            if (room->inputs[peer].sampleReady()) {
                REQUIRE(room->inputs[peer].sample(
                    room->commands(peer, room->inputs[peer].sampleTick(), false)));
            }
        }
        pumpAgreement(*room);
        for (const u8 peer : {u8{1}, u8{2}}) {
            if (room->inputs[peer].advance()) {
                REQUIRE(room->inputs[peer].complete());
            }
        }
    };
    for (u32 attempt = 0; attempt < 12; ++attempt) {
        stepPair();
    }
    CHECK(room->sessions[1].quietPeers() == std::vector<u8>{3});
    CHECK(room->sessions[2].quietPeers() == std::vector<u8>{3});
    REQUIRE(room->sessions[1].drop(3));
    pumpAgreement(*room);
    CHECK(room->sessions[3].phase() == Phase::Stopped);
    CHECK(room->sessions[3].stopReason() == MatchStop::Left);
    for (const u8 peer : {u8{1}, u8{2}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        const auto drops = room->sessions[peer].takeDrops();
        REQUIRE(drops.size() == 1);
        CHECK(drops.front().peer == 3);
        CHECK(drops.front().tick == room->sessions[1].dropsFor(drops.front().epoch).front().tick);
        REQUIRE(room->inputs[peer].inputs().suspend(3, drops.front().tick));
    }
    CHECK(room->sessions[1].quietPeers().empty());
    // The two go on and agree at each cadence without peer three.
    for (const u64 cadence : {u64{30}, u64{60}}) {
        CAPTURE(cadence);
        for (u32 attempt = 0; attempt < 200 && (room->inputs[1].confirmedTick() < cadence ||
                                                room->inputs[2].confirmedTick() < cadence);
             ++attempt) {
            stepPair();
        }
        for (const u8 peer : {u8{1}, u8{2}}) {
            CAPTURE(peer);
            REQUIRE(room->inputs[peer].confirmedTick() >= cadence);
            REQUIRE(room->sessions[peer].observeNative(cadence, observed(), MatchSession::Time{}));
        }
        pumpAgreement(*room);
        for (const u8 peer : {u8{1}, u8{2}}) {
            CAPTURE(peer);
            CHECK(room->sessions[peer].phase() == Phase::Running);
            REQUIRE(room->sessions[peer].nativeAgreement());
            CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == cadence);
        }
    }
}

TEST_CASE("a host observation mismatch orders the diverged peer to rebuild and the match goes on",
          "[match-lockstep][match-native-agreement][journal]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 30);
    REQUIRE(room->sessions[1].observeNative(30, observed(), MatchSession::Time{}));
    REQUIRE(room->sessions[2].observeNative(30, observed(1), MatchSession::Time{}));
    REQUIRE(room->sessions[3].observeNative(30, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    CHECK(room->sessions[1].phase() == Phase::Running);
    CHECK_FALSE(room->sessions[1].resyncing());
    REQUIRE(room->sessions[1].nativeAgreement());
    CHECK(room->sessions[1].nativeAgreement()->agreedTick() == 0);
    REQUIRE(room->sessions[4].observeNative(30, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    // Nobody stops: the host names peer two and restarts agreement at tick 30 everywhere.
    for (u8 peer = 1; peer <= 4; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        REQUIRE(room->sessions[peer].resyncing());
        CHECK(room->sessions[peer].resync()->tick == 30);
        CHECK(room->sessions[peer].resync()->peers == 0b100);
        REQUIRE(room->sessions[peer].nativeAgreement());
        CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == 30);
        CHECK(room->inputs[peer].sampleReady());
    }
    const auto pending = room->sessions[1].pendingResync();
    REQUIRE(pending);
    CHECK(pending->tick == 30);
    CHECK(pending->peers == 0b100);
    CHECK(room->sessions[2].resyncRequested() == 30);
    CHECK_FALSE(room->sessions[3].resyncRequested());
    CHECK_FALSE(room->sessions[2].journalReady());
    // The host alone sends the named peer the journal of the thirty confirmed ticks.
    EpochInputJournal journal;
    REQUIRE(journal.open(room->sessions[1].context().epoch, room->owners));
    for (u64 tick = 1; tick <= 30; ++tick) {
        JournalFrame frame;
        for (u8 peer = 1; peer <= 4; ++peer) {
            for (const auto& command : room->commands(peer, tick, false)) {
                frame[command.seat] = command;
            }
        }
        REQUIRE(journal.append(tick, frame));
    }
    const auto chunks = JournalPacket::chunks(journal, 1, 30, true);
    REQUIRE_FALSE(chunks.empty());
    CHECK_FALSE(room->sessions[2].queueJournal(2, chunks.front()));
    CHECK_FALSE(room->sessions[1].queueJournal(3, chunks.front()));
    for (const auto& chunk : chunks) {
        REQUIRE(room->sessions[1].queueJournal(2, chunk));
    }
    CHECK(room->sessions[1].queuedJournal() == chunks.size());
    REQUIRE(room->sessions[1].journalQueued(2));
    CHECK_FALSE(room->sessions[1].pendingResync());
    CHECK_FALSE(room->sessions[1].queueJournal(2, chunks.front()));
    pumpAgreement(*room);
    CHECK(room->sessions[1].queuedJournal() == 0);
    REQUIRE(room->sessions[2].journalReady());
    const auto commands = room->sessions[2].takeJournal();
    CHECK(commands.size() == usize{30} * 4);
    CHECK(commands.front().tick == 1);
    CHECK(commands.back().tick == 30);
    CHECK_FALSE(room->sessions[2].journalReady());
    REQUIRE(room->sessions[2].finishResync());
    CHECK_FALSE(room->sessions[2].resyncing());
    CHECK_FALSE(room->sessions[2].finishResync());
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK_FALSE(room->sessions[peer].resyncing());
    }
    // Agreement continues from the restart with everyone in step.
    executeThrough(*room, 60);
    for (u8 peer = 1; peer <= 4; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(60, observed(), MatchSession::Time{}));
    }
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        REQUIRE(room->sessions[peer].nativeAgreement());
        CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == 60);
    }
}

TEST_CASE("a rebuilt guest hashes again the ticks its diverged scene had already hashed",
          "[match-lockstep][match-native-agreement][journal]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 60);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(30, observed(peer == 2 ? 1 : 0),
                                                   MatchSession::Time{}));
    }
    // The verdict on tick 30 comes once every peer has also hashed tick 60, the diverged
    // guest wrongly.
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(60, observed(peer == 2 ? 1 : 0),
                                                   MatchSession::Time{}));
    }
    pumpAgreement(*room);
    auto& guest = room->sessions[2];
    REQUIRE(guest.resyncRequested() == 30);
    REQUIRE(guest.finishResync());
    pumpAgreement(*room);
    // Rebuilt, the guest reaches tick 60 again and hashes it as everyone else did.
    REQUIRE(guest.publishNativeInputs({}, 61));
    REQUIRE(guest.confirmNativeTick(guest.context().epoch, 60));
    CHECK(guest.observeNative(60, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK_FALSE(room->sessions[peer].resyncing());
        REQUIRE(room->sessions[peer].nativeAgreement());
        CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == 60);
    }
}

TEST_CASE("a rebuild target dropped mid-repair leaves nothing owed and the rest carry on",
          "[match-lockstep][match-native-agreement][journal][drop]") {
    const bool transferStarted = GENERATE(false, true);
    const bool linkLost = GENERATE(false, true);
    CAPTURE(transferStarted, linkLost);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 30);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(30, observed(peer == 2 ? 1 : 0),
                                                   MatchSession::Time{}));
    }
    pumpAgreement(*room);
    auto& host = room->sessions[1];
    REQUIRE(host.pendingResync());
    REQUIRE(host.resync()->peers == 0b100);
    EpochInputJournal journal;
    REQUIRE(journal.open(host.context().epoch, room->owners));
    for (u64 tick = 1; tick <= 30; ++tick) {
        JournalFrame frame;
        for (u8 peer = 1; peer <= 3; ++peer) {
            for (const auto& command : room->commands(peer, tick, false)) {
                frame[command.seat] = command;
            }
        }
        REQUIRE(journal.append(tick, frame));
    }
    const auto chunks = JournalPacket::chunks(journal, 1, 30, true);
    REQUIRE_FALSE(chunks.empty());
    if (transferStarted) {
        for (const auto& chunk : chunks) {
            REQUIRE(host.queueJournal(2, chunk));
        }
        REQUIRE(host.journalQueued(2));
    }
    // The target's link is lost, or the stall deadline drops it while the link stands.
    REQUIRE((linkLost ? host.disconnected(102) : host.drop(2)));
    REQUIRE(host.dropped(2));
    CHECK(host.queuedJournal() == 0);
    CHECK_FALSE(host.queueJournal(2, chunks.front()));
    pumpAgreement(*room);
    for (const u8 peer : {u8{1}, u8{3}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK(room->sessions[peer].dropped(2));
        CHECK_FALSE(room->sessions[peer].resyncing());
        CHECK_FALSE(room->sessions[peer].pendingResync());
    }
}

TEST_CASE("the stage after a guest's drop opens, skips its movie and plays without that guest",
          "[match-lockstep][drop][native-travel]") {
    const auto owners =
        GENERATE(MatchOwners{1, 2, 0, 0}, MatchOwners{1, 2, 3, 0}, MatchOwners{1, 2, 3, 4});
    auto room = std::make_unique<Room>(owners);
    room->ready();
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (room->present(peer)) {
            REQUIRE(room->inputs[peer].sample(room->commands(peer, 1, false)));
        }
    }
    room->pump();
    room->advanceAll(1, false);
    room->agreeTerminal();
    auto& host = room->sessions[1];
    // The guest goes before the host prepares, or while the others are still being told to
    // and are finishing the stage it leaves.
    const bool afterPrepare = GENERATE(false, true);
    CAPTURE(afterPrepare);
    const auto party = room->party();
    if (!afterPrepare) {
        REQUIRE(host.disconnected(102));
    }
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    if (afterPrepare) {
        REQUIRE(host.disconnected(102));
    }
    REQUIRE(host.dropped(2));
    room->pump();
    std::vector<u8> survivors;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (room->present(peer) && peer != 2) {
            survivors.push_back(peer);
        }
    }
    for (const u8 peer : survivors) {
        CAPTURE(peer);
        if (peer != 1) {
            REQUIRE(room->inputs[peer].completeBoundary());
        }
        REQUIRE(room->sessions[peer].phase() == Phase::Loading);
        REQUIRE(room->sessions[peer].context() == host.context());
    }
    // Everyone left asks to skip the entry movie; the dropped guest's seat counts as asking.
    for (const u8 peer : survivors) {
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] == peer) {
                REQUIRE(room->sessions[peer].requestMovieSkip(seat));
            }
        }
    }
    room->pump();
    for (const u8 peer : survivors) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].movieSkipReady());
        REQUIRE(room->loaded(peer));
    }
    room->pump();
    for (const u8 peer : survivors) {
        CAPTURE(peer);
        REQUIRE(room->sessions[peer].phase() == Phase::Running);
        const auto drops = room->sessions[peer].dropsFor(host.context().epoch);
        REQUIRE(drops.size() == 1);
        CHECK(drops.front().tick == 1);
        REQUIRE(room->inputs[peer].inputs().suspend(2, drops.front().tick));
    }
    // The dropped guest's seat holds still from the first tick, and nobody waits for it.
    for (u64 tick = 1; tick <= 5; ++tick) {
        for (const u8 peer : survivors) {
            REQUIRE(room->inputs[peer].sample(room->commands(peer, tick, false)));
        }
        room->pump();
        for (const u8 peer : survivors) {
            CAPTURE(peer, tick);
            const auto frame = room->inputs[peer].advance();
            REQUIRE(frame);
            CHECK(frame->predictedSeats == 0);
            CHECK(frame->commands[1].magnitude == 0.0f);
            REQUIRE(room->inputs[peer].complete());
        }
    }
}

TEST_CASE("the host ignores what a rebuilding guest reported before it was told to rebuild",
          "[match-lockstep][match-native-agreement][journal]") {
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 60);
    // The guest's report on sixty follows its wrong one on thirty, so it reaches the host
    // only after the host has restarted agreement and ordered the rebuild.
    for (u8 peer = 1; peer <= 2; ++peer) {
        for (const u64 tick : {u64{30}, u64{60}}) {
            REQUIRE(room->sessions[peer].observeNative(tick, observed(peer == 2 ? 1 : 0),
                                                       MatchSession::Time{}));
        }
    }
    pumpAgreement(*room);
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.resync());
    CHECK(host.resync()->tick == 30);
    REQUIRE(guest.resyncRequested() == 30);
    REQUIRE(guest.finishResync());
    pumpAgreement(*room);
    // Rebuilt, it confirms sixty again and hashes what the host hashed there.
    REQUIRE(room->inputs[2].complete());
    REQUIRE(guest.observeNative(60, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 2; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK_FALSE(room->sessions[peer].resyncing());
        REQUIRE(room->sessions[peer].nativeAgreement());
        CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == 60);
    }
}

TEST_CASE("a mismatch the host's own late digest completes is repaired rather than ending play",
          "[match-lockstep][match-native-agreement][journal]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 30);
    // Both guests report thirty before the host's own scene has hashed it; guest two's differs.
    REQUIRE(room->sessions[2].observeNative(30, observed(1), MatchSession::Time{}));
    REQUIRE(room->sessions[3].observeNative(30, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    auto& host = room->sessions[1];
    REQUIRE_FALSE(host.resyncing());
    REQUIRE(host.observeNative(30, observed(), MatchSession::Time{}));
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.resync());
    CHECK(host.resync()->tick == 30);
    CHECK(host.resync()->peers == 0b100);
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
    }
    CHECK(room->sessions[2].resyncRequested() == 30);
    CHECK_FALSE(room->sessions[3].resyncRequested());
}

TEST_CASE("a drop that leaves the remaining digests disagreeing orders a repair, not an end",
          "[match-lockstep][match-native-agreement][journal][drop]") {
    // The next update orders the repair, or else the next flush before the failed stream's
    // notice could leave.
    const bool updated = GENERATE(false, true);
    CAPTURE(updated);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 30);
    auto& host = room->sessions[1];
    REQUIRE(host.observeNative(30, observed(), MatchSession::Time{}));
    REQUIRE(room->sessions[2].observeNative(30, observed(1), MatchSession::Time{}));
    pumpAgreement(*room);
    // Guest three never reports thirty; dropping it leaves only the disagreement.
    REQUIRE_FALSE(host.resyncing());
    REQUIRE(host.drop(3));
    if (updated) {
        host.update(0.0, MatchSession::Time{});
        REQUIRE(host.resync());
    }
    pumpAgreement(*room);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.resync());
    CHECK(host.resync()->tick == 30);
    CHECK(host.resync()->peers == 0b100);
    CHECK(room->sessions[2].phase() == Phase::Running);
    CHECK(room->sessions[2].resyncRequested() == 30);
}

TEST_CASE("after a rebuild target is dropped mid-repair the next divergence is repaired too",
          "[match-lockstep][match-native-agreement][journal][drop]") {
    const bool linkLost = GENERATE(false, true);
    CAPTURE(linkLost);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->ready();
    executeThrough(*room, 30);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(30, observed(peer == 2 ? 1 : 0),
                                                   MatchSession::Time{}));
    }
    pumpAgreement(*room);
    auto& host = room->sessions[1];
    REQUIRE(host.resyncing());
    REQUIRE(host.resync()->peers == 0b100);
    // The target's link goes, or the stall deadline drops it, before it reports a rebuild.
    REQUIRE((linkLost ? host.disconnected(102) : host.drop(2)));
    pumpAgreement(*room);
    for (const u8 peer : {u8{1}, u8{3}}) {
        CAPTURE(peer);
        REQUIRE_FALSE(room->sessions[peer].resyncing());
        const auto drops = room->sessions[peer].takeDrops();
        REQUIRE(drops.size() == 1);
        REQUIRE(room->inputs[peer].inputs().suspend(2, drops.front().tick));
    }
    for (u32 attempt = 0; attempt < 200 && (room->inputs[1].confirmedTick() < 60 ||
                                            room->inputs[3].confirmedTick() < 60);
         ++attempt) {
        for (const u8 peer : {u8{1}, u8{3}}) {
            if (room->inputs[peer].sampleReady()) {
                REQUIRE(room->inputs[peer].sample(
                    room->commands(peer, room->inputs[peer].sampleTick(), false)));
            }
        }
        pumpAgreement(*room);
        for (const u8 peer : {u8{1}, u8{3}}) {
            if (room->inputs[peer].advance()) {
                REQUIRE(room->inputs[peer].complete());
            }
        }
    }
    REQUIRE(room->sessions[1].observeNative(60, observed(), MatchSession::Time{}));
    REQUIRE(room->sessions[3].observeNative(60, observed(3), MatchSession::Time{}));
    pumpAgreement(*room);
    // A repair left waiting on the dropped peer would make this mismatch end the match.
    for (const u8 peer : {u8{1}, u8{3}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        REQUIRE(room->sessions[peer].resyncing());
        CHECK(room->sessions[peer].resync()->tick == 60);
        CHECK(room->sessions[peer].resync()->peers == 0b1000);
    }
    CHECK(room->sessions[3].resyncRequested() == 60);
}

TEST_CASE("local input conflicts report history failure instead of a connection failure",
          "[match-lockstep][netplay-diagnostics]") {
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 30);
    auto& guest = room->sessions[2];
    REQUIRE(guest.publishNativeInputs(room->commands(2, 31, true), 31));
    CHECK_FALSE(guest.publishNativeInputs(room->commands(2, 31, false), 31));
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::TransportFailure);
    CHECK(guest.failureDetail().find("input publication failed: conflicting input history") !=
          std::string_view::npos);
    CHECK(guest.failureDetail().find("epoch 1, peer 2, next tick 31") != std::string_view::npos);
    const auto detail = std::string(guest.failureDetail());
    guest.leave();
    CHECK(guest.failureDetail() == detail);
}

TEST_CASE("remote input conflicts report the origin and receiving simulation tick",
          "[match-lockstep][netplay-diagnostics]") {
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 30);
    auto& host = room->sessions[1];
    const auto contradictory = inputMessage(2, room->commands(2, 30, true), 31, 10000);
    CHECK(host.receive(102, contradictory) == Admission::Invalid);
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.failureDetail().find("input reception failed: conflicting input history") !=
          std::string_view::npos);
    CHECK(host.failureDetail().find("epoch 1, peer 1, origin 2, next tick 31") !=
          std::string_view::npos);
}

TEST_CASE("native match guest observation failure retains shared stop through congestion",
          "[match-lockstep][match-native-agreement]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 30);
    auto& guest = room->sessions[2];
    REQUIRE(guest.observeNative(30, observed(), MatchSession::Time{}));
    room->wires[2].result = PacketTransport::SendResult::Congested;
    guest.flush(room->wires[2], MatchSession::Time{});
    CHECK_FALSE(guest.observeNative(30, observed(1), MatchSession::Time{}));
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::TransportFailure);
    CHECK(guest.queuedControls() == 1);
    for (u32 attempt = 0; attempt < 10; ++attempt) {
        guest.flush(room->wires[2], MatchSession::Time{});
        CHECK(guest.queuedControls() == 1);
    }
    CHECK(room->wires[2].sent.empty());
    room->wires[2].result = PacketTransport::SendResult::Sent;
    pumpAgreement(*room);
    // The retained stop arrives at last: the host drops that guest and the rest play on.
    for (const u8 peer : {u8{1}, u8{3}, u8{4}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK(room->sessions[peer].dropped(2));
    }
}

TEST_CASE("native match observation timeout uses monotonic time even while simulation is stalled",
          "[match-lockstep][match-native-agreement]") {
    const bool congested = GENERATE(false, true);
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 30);
    auto& guest = room->sessions[2];
    REQUIRE(guest.observeNative(30, observed(), MatchSession::Time{}));
    if (congested) {
        room->wires[2].result = PacketTransport::SendResult::Congested;
    }
    pumpAgreement(*room);
    guest.update(0, MatchSession::Time{} + std::chrono::seconds{9});
    CHECK(guest.phase() == Phase::Running);
    // The host's own digest is late but in before the guest gives up.
    REQUIRE(room->sessions[1].observeNative(30, observed(),
                                            MatchSession::Time{} + std::chrono::seconds{9}));
    guest.update(0, MatchSession::Time{} + NativeAgreement::kTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::TransportFailure);
    room->wires[2].result = PacketTransport::SendResult::Sent;
    pumpAgreement(*room, MatchSession::Time{} + NativeAgreement::kTimeout);
    // Its stop reaches the host, which drops it and agrees the tick alone.
    CHECK(room->sessions[1].phase() == Phase::Running);
    CHECK(room->sessions[1].dropped(2));
    REQUIRE(room->sessions[1].nativeAgreement());
    CHECK(room->sessions[1].nativeAgreement()->agreedTick() == 30);
}

TEST_CASE("native match observation demux preserves authentication and old epoch isolation",
          "[match-lockstep][match-native-agreement]") {
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    // Use a real earlier nonzero epoch. Epoch zero is malformed, not a stale
    // agreement stream, and cannot be produced by the native sender.
    const auto previousEpoch = host.context().epoch;
    const auto previousReport = observationReport(previousEpoch, observed());
    executeThrough(*room, 1);
    const auto initialParty = room->party();
    room->agreeTerminal(MatchSession::Time{});
    REQUIRE(host.prepare(7, MatchTransition::Travel, &initialParty));
    pumpAgreement(*room);
    REQUIRE(room->sessions[2].nativeDraining());
    REQUIRE(room->inputs[2].completeBoundary());
    room->commit();
    const auto epoch = host.context().epoch;
    REQUIRE(epoch == previousEpoch + 1);
    const auto report = observationReport(epoch, observed());
    CHECK(host.receive(999, report, MatchSession::Time::max()) == Admission::WrongPeer);
    CHECK(host.nativeAgreement() == nullptr);
    CHECK(host.receive(102, previousReport, MatchSession::Time::max()) == Admission::Stale);
    CHECK(host.nativeAgreement() == nullptr);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.receive(102, report, MatchSession::Time{}) == Admission::Accepted);
    REQUIRE(host.nativeAgreement());
    CHECK(host.nativeAgreement()->agreedTick() == 0);
    executeThrough(*room, 30);
    REQUIRE(host.observeNative(30, observed(), MatchSession::Time{}));
    // The synthetic received report is authenticated evidence, but the guest
    // still needs its own local submission to validate the host's verdict.
    REQUIRE(room->sessions[2].observeNative(30, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    CHECK(host.nativeAgreement()->agreedTick() == 30);
    const auto party = room->party();
    room->agreeTerminal(MatchSession::Time{});
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.nativeAgreement() == nullptr);
    pumpAgreement(*room);
    REQUIRE(room->sessions[2].nativeDraining());
    REQUIRE(room->inputs[2].completeBoundary());
    CHECK(room->sessions[2].nativeAgreement() == nullptr);
    room->commit();
    CHECK(host.context().epoch == epoch + 1);
    CHECK(host.receive(102, report, MatchSession::Time::max()) == Admission::Stale);
    CHECK(host.nativeAgreement() == nullptr);
    executeThrough(*room, 30);
    for (u8 peer = 1; peer <= 2; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(30, observed(2), MatchSession::Time{}));
    }
    pumpAgreement(*room);
    CHECK(host.nativeAgreement()->agreedTick() == 30);
    CHECK(host.phase() == Phase::Running);
}

TEST_CASE("native match missing scheduled observations and malformed current reports fail closed",
          "[match-lockstep][match-native-agreement]") {
    const bool missing = GENERATE(false, true);
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    if (missing) {
        executeThrough(*room, 60); // optional until used, but cannot start by omitting tick 30
        CHECK_FALSE(host.observeNative(60, observed(), MatchSession::Time{}));
    } else {
        auto malformed = observationReport(host.context().epoch, observed());
        malformed[7] = 1;
        CHECK(host.receive(102, malformed, MatchSession::Time{}) == Admission::Invalid);
    }
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::TransportFailure);
    pumpAgreement(*room);
    CHECK(room->sessions[2].phase() == Phase::Stopped);
    CHECK(room->sessions[2].stopReason() == MatchStop::TransportFailure);
}

TEST_CASE("a guest's failed transport or lost link drops it and the rest agree without it",
          "[match-lockstep][match-native-agreement][drop]") {
    const bool disconnected = GENERATE(false, true);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 30);
    REQUIRE(room->sessions[1].observeNative(30, observed(), MatchSession::Time{}));
    if (disconnected) {
        REQUIRE(room->sessions[1].disconnected(102));
    } else {
        // The guest's own stop reaches the host instead of a closed link.
        auto& guest = room->sessions[2];
        REQUIRE(guest.observeNative(30, observed(), MatchSession::Time{}));
        room->wires[2].result = PacketTransport::SendResult::Invalid;
        guest.flush(room->wires[2], MatchSession::Time{});
        CHECK(guest.phase() == Phase::Stopped);
        CHECK(guest.stopReason() == MatchStop::TransportFailure);
        room->wires[2].result = PacketTransport::SendResult::Sent;
        pumpAgreement(*room);
    }
    // The lost guest is dropped, and the agreement it owed settles without it.
    CHECK(room->sessions[1].phase() == Phase::Running);
    CHECK(room->sessions[1].dropped(2));
    pumpAgreement(*room);
    for (const u8 peer : {u8{3}, u8{4}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        CHECK(room->sessions[peer].dropped(2));
        REQUIRE(room->sessions[peer].observeNative(30, observed(), MatchSession::Time{}));
    }
    pumpAgreement(*room);
    for (const u8 peer : {u8{1}, u8{3}, u8{4}}) {
        CAPTURE(peer);
        CHECK(room->sessions[peer].phase() == Phase::Running);
        REQUIRE(room->sessions[peer].nativeAgreement());
        CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == 30);
    }
}

namespace {
/** Room::pump with guests two and three also linked to each other directly. Nothing moves
 * to or from a peer the host has dropped. */
void pumpLinked(Room& room) {
    for (u8 pass = 0; pass < 4; ++pass) {
        for (u8 source = 1; source <= 3; ++source) {
            if (room.sessions[1].dropped(source)) {
                continue;
            }
            room.sessions[source].flush(room.wires[source]);
            const auto packets = std::move(room.wires[source].sent);
            room.wires[source].sent.clear();
            for (const auto& packet : packets) {
                const u8 target = source == 1 ? static_cast<u8>(packet.connection - 100)
                                  : packet.connection == kHost ? u8{1}
                                  : source == 2                ? u8{3}
                                                               : u8{2};
                if (room.sessions[1].dropped(target)) {
                    continue;
                }
                const auto incoming = source == 1   ? kHost
                                      : target == 1 ? PacketTransport::Connection{100} + source
                                      : target == 3 ? PacketTransport::Connection{302}
                                                    : PacketTransport::Connection{203};
                const auto admitted = room.sessions[target].receive(incoming, packet.bytes);
                CAPTURE(source, target, static_cast<unsigned>(admitted));
                REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
            }
        }
    }
}
} // namespace

TEST_CASE("a drop gives every survivor the host's still seats whatever a direct link brought",
          "[match-lockstep][drop][direct-link]") {
    const bool executed = GENERATE(false, true);
    CAPTURE(executed);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    const std::array hostLink{MatchLink{1, kHost}};
    const std::array toThird{MatchLink{3, 203}};
    const std::array toSecond{MatchLink{2, 302}};
    room->sessions[2].clear();
    room->sessions[3].clear();
    REQUIRE(room->sessions[2].open(2, room->owners, hostLink, 0, toThird));
    REQUIRE(room->sessions[3].open(3, room->owners, hostLink, 0, toSecond));
    REQUIRE(room->sessions[1].prepare(7));
    pumpLinked(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->loaded(peer));
    }
    pumpLinked(*room);
    auto& third = room->inputs[3];
    // Each advance passes on the host's receipt first (reportHostReceipts), as the shipping
    // loop does.
    const auto witness = [&] { REQUIRE(room->sessions[3].hostReceipt(2)); };
    for (u64 tick = 1; tick <= 30; ++tick) {
        for (u8 peer = 1; peer <= 3; ++peer) {
            REQUIRE(room->inputs[peer].sample(room->commands(peer, tick, false)));
        }
        pumpLinked(*room);
        witness();
        room->advanceAll(tick, false);
    }
    CHECK(room->sessions[3].hostReceipt(2) == 30);
    CHECK_FALSE(room->sessions[3].hostReceipt(1)); // the host's own input is all its own
    CHECK_FALSE(room->sessions[1].hostReceipt(2)); // and the host hears no guest directly
    for (const u8 peer : {u8{1}, u8{3}}) {
        REQUIRE(room->inputs[peer].sample(room->commands(peer, 31, false)));
    }
    // Peer two's next movement reaches peer three directly; its copy to the host is lost.
    const auto early = inputMessage(2, room->commands(2, 31, true), 32, 1000);
    REQUIRE(room->sessions[3].receive(302, early) == Admission::Accepted);
    if (executed) {
        // The host's own tick 31 reaches peer three as well; peer two sent nothing more.
        room->sessions[1].flush(room->wires[1]);
        const auto fromHost = std::move(room->wires[1].sent);
        room->wires[1].sent.clear();
        for (const auto& packet : fromHost) {
            if (packet.connection == 103) {
                const auto admitted = room->sessions[3].receive(kHost, packet.bytes);
                REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
            }
        }
        witness();
        const auto walked = third.advance();
        REQUIRE(walked);
        CHECK(walked->commands[1].magnitude == 0.5f);
        REQUIRE(third.complete());
        CHECK(third.confirmedTick() == 30); // the host holds peer two only through tick 30
    }
    REQUIRE(room->sessions[1].disconnected(102));
    pumpLinked(*room);
    for (const u8 peer : {u8{1}, u8{3}}) {
        const auto drops = room->sessions[peer].takeDrops();
        REQUIRE(drops.size() == 1);
        REQUIRE(drops.front().tick == 31);
        REQUIRE(room->inputs[peer].inputs().suspend(2, 31));
    }
    witness();
    REQUIRE(room->inputs[1].advance());
    const auto taken = third.advance(); // corrects tick 31 if it walked there, else takes it
    CHECK(taken.has_value() == !executed);
    REQUIRE(third.lastFrame());
    CHECK(third.lastFrame()->commands[1].magnitude == 0.0f);
    CHECK(third.tick() == 32);
    for (u64 tick = 1; tick <= 31; ++tick) {
        CAPTURE(tick);
        const auto hostFrame = room->inputs[1].inputs().actualFrame(tick);
        const auto guestFrame = third.inputs().actualFrame(tick);
        REQUIRE(hostFrame);
        REQUIRE(guestFrame);
        for (usize seat = 0; seat < 3; ++seat) {
            CHECK(inputBytes(std::span{&(*hostFrame)[seat], 1}) ==
                  inputBytes(std::span{&(*guestFrame)[seat], 1}));
        }
    }
    CHECK(third.confirmedTick() == 31); // a dropped guest bounds nothing any more
    REQUIRE(third.complete());
}

TEST_CASE("a guest still linked to a dropped guest opens the next stage without that link",
          "[match-lockstep][drop][direct-link][native-travel]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    const std::array hostLink{MatchLink{1, kHost}};
    const std::array toThird{MatchLink{3, 203}};
    const std::array toSecond{MatchLink{2, 302}};
    room->sessions[2].clear();
    room->sessions[3].clear();
    REQUIRE(room->sessions[2].open(2, room->owners, hostLink, 0, toThird));
    REQUIRE(room->sessions[3].open(3, room->owners, hostLink, 0, toSecond));
    REQUIRE(room->sessions[1].prepare(7));
    pumpLinked(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->loaded(peer));
    }
    pumpLinked(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->inputs[peer].sample(room->commands(peer, 1, false)));
    }
    pumpLinked(*room);
    room->advanceAll(1, false);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->inputs[peer].submitBoundaryDigest());
    }
    pumpLinked(*room);
    for (u8 peer = 1; peer <= 3; ++peer) {
        REQUIRE(room->inputs[peer].completeBoundary());
    }
    auto& host = room->sessions[1];
    auto& third = room->sessions[3];
    REQUIRE(host.disconnected(102));
    const auto party = room->party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    pumpLinked(*room);
    REQUIRE(room->inputs[3].completeBoundary());
    REQUIRE(third.phase() == Phase::Loading);
    REQUIRE(third.context() == host.context());
    CHECK(third.dropped(2));
    // Nothing has told guest three that its own link to guest two is gone.
    REQUIRE(third.directLinks() == 1);
    for (const u8 peer : {u8{1}, u8{3}}) {
        for (u8 seat = 0; seat < static_cast<u8>(room->owners.size()); ++seat) {
            if (room->owners[seat] == peer) {
                REQUIRE(room->sessions[peer].requestMovieSkip(seat));
            }
        }
    }
    pumpLinked(*room);
    for (const u8 peer : {u8{1}, u8{3}}) {
        CAPTURE(peer);
        REQUIRE(room->loaded(peer));
    }
    pumpLinked(*room);
    for (const u8 peer : {u8{1}, u8{3}}) {
        CAPTURE(peer);
        REQUIRE(room->sessions[peer].phase() == Phase::Running);
        const auto drops = room->sessions[peer].dropsFor(host.context().epoch);
        REQUIRE(drops.size() == 1);
        REQUIRE(room->inputs[peer].inputs().suspend(2, drops.front().tick));
    }
    for (u64 tick = 1; tick <= 5; ++tick) {
        for (const u8 peer : {u8{1}, u8{3}}) {
            REQUIRE(room->inputs[peer].sample(room->commands(peer, tick, false)));
        }
        // The new stage's input goes to the host alone, never down the stale link.
        third.flush(room->wires[3]);
        for (const auto& packet : room->wires[3].sent) {
            CHECK(packet.connection == kHost);
        }
        pumpLinked(*room);
        for (const u8 peer : {u8{1}, u8{3}}) {
            CAPTURE(peer, tick);
            const auto frame = room->inputs[peer].advance();
            REQUIRE(frame);
            CHECK(frame->predictedSeats == 0);
            CHECK(frame->commands[1].magnitude == 0.0f);
            REQUIRE(room->inputs[peer].complete());
        }
    }
}
