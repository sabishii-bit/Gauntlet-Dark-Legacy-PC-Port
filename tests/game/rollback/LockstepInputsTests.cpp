#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "fixtures/PacketStar.h"
#include "game/rollback/LockstepInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = LockstepInputs::Admission;
constexpr u64 kEpoch = 17;
constexpr PacketTransport::Connection kHostConnection = 99;
constexpr test::StarLinks kStar{kHostConnection};

std::vector<u8> packet(std::span<const InputCommand> commands) {
    const auto encoded = InputPacket::encode(commands);
    REQUIRE(encoded);
    return *encoded;
}

bool sameCommand(const InputCommand& left, const InputCommand& right) {
    return packet(std::span{&left, 1}) == packet(std::span{&right, 1});
}

struct Room : test::PacketStar<> {
    LockstepInputs::Grants grants{};
    std::array<LockstepInputs, kSlots> inputs;

    explicit Room(LockstepInputs::Owners roster = {1, 2, 0, 0}, u8 delay = 0)
        : PacketStar(roster, kStar) {
        for (usize seat = 0; seat < owners.size(); ++seat) {
            grants[seat] = owners[seat] != 0 ? static_cast<u32>(10 + seat) : 0;
        }
        std::vector<LockstepInputs::Link> guests;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                guests.push_back({peer, connection(peer)});
                const std::array host{LockstepInputs::Link{1, kHostConnection}};
                REQUIRE(inputs[peer].open(peer, kEpoch, owners, grants, delay, host));
            }
        }
        REQUIRE(inputs[1].open(1, kEpoch, owners, grants, delay, guests));
    }

    static PacketTransport::Connection connection(u8 peer) { return kStar.guest(peer); }

    std::vector<InputCommand> commands(u8 peer, u64 tick, bool actions = false) const {
        std::vector<InputCommand> result;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] != peer) {
                continue;
            }
            InputCommand command;
            command.epoch = kEpoch;
            command.tick = tick;
            command.grant = grants[seat];
            command.seat = seat;
            if (actions) {
                command.direction = (tick + seat) % 2 == 0 ? Vec2{1, 0} : Vec2{0, 1};
                command.magnitude = 0.5f;
                command.aimDirection = Vec3{0, 0, 1};
                command.heldButtons = static_cast<u32>(CommandHeld::Attack);
                if (tick % 3 == 0) {
                    command.heldButtons |= static_cast<u32>(CommandHeld::ThrowPotion);
                }
                if (tick % 5 == 0) {
                    command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
                }
            }
            result.push_back(command);
        }
        return result;
    }

    /** What a delivered input packet meets: its checks, then its receiver, which must take it. */
    auto arrival() {
        return [this](u8 source, u8 target, PacketTransport::Connection sender,
                      const Packet& message) {
            REQUIRE(message.delivery == PacketTransport::Delivery::Reliable);
            REQUIRE(message.bytes.size() <= PacketTransport::kMaxPacketBytes);
            CAPTURE(source, target);
            REQUIRE(inputs[target].receive(sender, message.bytes) == Admission::Accepted);
        };
    }

    void deliver(u8 source) { PacketStar::deliver(source, arrival()); }

    void pump() {
        // The second pass carries host relays produced after its first flush.
        // No simulation is advanced in this transport-only operation.
        PacketStar::pump(
            2, [this](u8 peer, PacketTransport& wire) { REQUIRE(inputs[peer].flush(wire)); },
            arrival());
    }

    void advanceAll(bool actions) {
        const auto tick = inputs[1].tick();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (!present(peer)) {
                continue;
            }
            REQUIRE(inputs[peer].ready());
            const auto frame = inputs[peer].advance();
            REQUIRE(frame);
            for (u8 source = 1; source <= 4; ++source) {
                if (present(source)) {
                    for (const auto& expected : commands(source, tick, actions)) {
                        CAPTURE(peer, tick, expected.seat);
                        CHECK(sameCommand((*frame)[expected.seat], expected));
                    }
                }
            }
            CHECK(inputs[peer].tick() == tick + 1);
        }
    }
};
} // namespace

TEST_CASE("lockstep requires an exact frozen authenticated star roster", "[lockstep-inputs]") {
    const u32 mutation = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U);
    CAPTURE(mutation);
    LockstepInputs inputs;
    u8 local = 1;
    u64 epoch = kEpoch;
    LockstepInputs::Owners owners{1, 2, 0, 0};
    LockstepInputs::Grants grants{10, 11, 0, 0};
    u8 delay = 0;
    std::vector<LockstepInputs::Link> links{{2, 102}};
    switch (mutation) {
    case 0: local = 0; break;
    case 1: local = 5; break;
    case 2: epoch = 0; break;
    case 3: delay = LockstepInputs::kMaxDelay + 1; break;
    case 4: owners[0] = 2; break;
    case 5: owners[1] = 5; break;
    case 6: grants[1] = 0; break;
    case 7: grants[2] = 1; break;
    case 8: links.clear(); break;
    case 9: links.front().connection = 0; break;
    case 10: links.push_back(links.front()); break;
    case 11: links.push_back({1, 101}); break;
    case 12:
        owners[2] = 3;
        grants[2] = 12;
        links.push_back({3, 102});
        break;
    default: break;
    }
    CHECK_FALSE(inputs.open(local, epoch, owners, grants, delay, links));
    CHECK_FALSE(inputs.ready());
    CHECK_FALSE(inputs.sampleReady());
    CHECK_FALSE(inputs.advance());
    CHECK(inputs.queuedPackets() == 0);
    // A rejected load barrier does not partially bind the instance.
    const std::array validLinks{LockstepInputs::Link{2, 102}};
    REQUIRE(inputs.open(1, kEpoch, {1, 2, 0, 0}, {10, 11, 0, 0}, 0, validLinks));
    CHECK_FALSE(inputs.open(1, kEpoch + 1, {1, 2, 0, 0}, {10, 11, 0, 0}, 0, validLinks));
}

TEST_CASE("lockstep guests cannot add direct peer links or omit their host", "[lockstep-inputs]") {
    LockstepInputs inputs;
    const LockstepInputs::Owners owners{1, 2, 3, 0};
    const LockstepInputs::Grants grants{10, 11, 12, 0};
    const std::array direct{LockstepInputs::Link{1, 99}, LockstepInputs::Link{3, 103}};
    CHECK_FALSE(inputs.open(2, kEpoch, owners, grants, 0, direct));
    CHECK_FALSE(inputs.open(2, kEpoch, owners, grants, 0, {}));
    const std::array host{LockstepInputs::Link{1, 99}};
    CHECK(inputs.open(2, kEpoch, owners, grants, 0, host));
}

TEST_CASE("lockstep neutral startup is exact for every agreed delay and never conceals later",
          "[lockstep-inputs]") {
    const auto delay = GENERATE(u8{0}, u8{1}, u8{2}, u8{3}, u8{4}, u8{5}, u8{6});
    CAPTURE(delay);
    Room room({1, 2, 0, 0}, delay);
    for (u64 tick = 1; tick <= delay; ++tick) {
        CHECK(room.inputs[1].queuedPackets() == 0);
        CHECK(room.inputs[2].queuedPackets() == 0);
        room.advanceAll(false);
    }
    for (u8 peer = 1; peer <= 2; ++peer) {
        CHECK(room.inputs[peer].sampleTick() == 1 + delay);
        CHECK_FALSE(room.inputs[peer].ready());
        CHECK_FALSE(room.inputs[peer].advance());
        REQUIRE(room.inputs[peer].sample(room.commands(peer, 1 + delay, true)));
        CHECK_FALSE(room.inputs[peer].ready());
    }
    room.pump();
    room.advanceAll(true);
    for (u8 peer = 1; peer <= 2; ++peer) {
        CHECK_FALSE(room.inputs[peer].advance());
        CHECK(room.inputs[peer].tick() == 2 + delay);
    }
}

TEST_CASE("lockstep delivers all simultaneous seats and one-shot edges byte exactly",
          "[lockstep-inputs]") {
    const auto owners =
        GENERATE(LockstepInputs::Owners{1, 2, 0, 0}, LockstepInputs::Owners{1, 2, 3, 0},
                 LockstepInputs::Owners{1, 2, 3, 4}, LockstepInputs::Owners{1, 2, 2, 3},
                 LockstepInputs::Owners{2, 0, 1, 2});
    const auto delay = GENERATE(u8{0}, u8{6});
    CAPTURE(owners, delay);
    Room room(owners, delay);
    for (u64 tick = 1; tick <= delay; ++tick) {
        room.advanceAll(false);
    }
    for (u64 tick = 1 + delay; tick <= 30 + delay; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room.present(peer)) {
                REQUIRE(room.inputs[peer].sampleReady());
                REQUIRE(room.inputs[peer].sample(room.commands(peer, tick, true)));
                CHECK_FALSE(room.inputs[peer].sample(room.commands(peer, tick, true)));
            }
        }
        room.pump();
        room.advanceAll(true);
    }
}

TEST_CASE("lockstep host relays a guest input before any host simulation step",
          "[lockstep-inputs]") {
    Room room({1, 2, 3, 0});
    const auto commands = room.commands(2, 1, true);
    const auto expected = packet(commands);
    REQUIRE(room.inputs[2].sample(commands));
    REQUIRE(room.inputs[2].flush(room.wires[2]));
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(room.wires[2].sent.front().bytes == expected);
    room.deliver(2);
    CHECK(room.inputs[1].tick() == 1);
    CHECK_FALSE(room.inputs[1].ready());
    REQUIRE(room.inputs[1].queuedPackets() == 1);
    REQUIRE(room.inputs[1].flush(room.wires[1]));
    REQUIRE(room.wires[1].sent.size() == 1);
    CHECK(room.wires[1].sent.front().connection == Room::connection(3));
    CHECK(room.wires[1].sent.front().bytes == expected);
    room.deliver(1);
    CHECK(room.inputs[3].tick() == 1);
    CHECK(room.inputs[3].queuedPackets() == 0);
    CHECK_FALSE(room.inputs[3].ready());
}

TEST_CASE("lockstep reordering cannot skip a missing complete frame", "[lockstep-inputs]") {
    Room room;
    auto& host = room.inputs[1];
    REQUIRE(host.sample(room.commands(1, 1, true)));
    REQUIRE(host.receive(102, packet(room.commands(2, 2, true))) == Admission::Accepted);
    for (u32 attempt = 0; attempt < 20; ++attempt) {
        CHECK_FALSE(host.advance());
        CHECK(host.tick() == 1);
        CHECK_FALSE(host.sampleReady());
    }
    REQUIRE(host.receive(102, packet(room.commands(2, 1, true))) == Admission::Accepted);
    const auto first = host.advance();
    REQUIRE(first);
    CHECK(sameCommand((*first)[1], room.commands(2, 1, true).front()));
    REQUIRE(host.sample(room.commands(1, 2, true)));
    const auto second = host.advance();
    REQUIRE(second);
    CHECK(sameCommand((*second)[1], room.commands(2, 2, true).front()));
    CHECK_FALSE(host.advance());
    CHECK(host.tick() == 3);
}

TEST_CASE("lockstep rejects wrong identities and partial multi-seat packets atomically",
          "[lockstep-inputs]") {
    Room room({1, 2, 2, 3});
    auto& host = room.inputs[1];
    const auto original = room.commands(2, 1, true);
    CHECK(host.receive(999, packet(original)) == Admission::WrongPeer);
    CHECK(host.receive(0, packet(original)) == Admission::WrongPeer);
    CHECK(host.receive(103, packet(original)) == Admission::WrongPeer);
    CHECK(host.receive(102, packet(room.commands(1, 1))) == Admission::WrongPeer);
    CHECK(host.receive(102, {}) == Admission::Invalid);
    auto malformed = packet(original);
    malformed.pop_back();
    CHECK(host.receive(102, malformed) == Admission::Invalid);
    malformed = packet(original);
    malformed[InputPacket::kHeaderBytes + 20] = 4; // out-of-range seat
    CHECK(host.receive(102, malformed) == Admission::Invalid);
    auto changed = original;
    changed.front().epoch = kEpoch + 1;
    CHECK(host.receive(102, packet(changed)) == Admission::Invalid);
    changed = original;
    ++changed.back().grant;
    CHECK(host.receive(102, packet(changed)) == Admission::Invalid);
    changed = original;
    ++changed.back().tick;
    CHECK(host.receive(102, packet(changed)) == Admission::Invalid);
    changed = original;
    changed.back() = changed.front();
    CHECK(host.receive(102, packet(changed)) == Admission::Invalid);
    CHECK(host.receive(102, packet(std::span{original}.first(1))) == Admission::Invalid);
    CHECK(host.queuedPackets() == 0);
    CHECK_FALSE(host.ready());
    REQUIRE(host.receive(102, packet(original)) == Admission::Accepted);
    CHECK(host.queuedPackets() == 1);
    // A host-authenticated relay may carry another peer's full roster, but a
    // guest's own controls can never be overwritten through that relay.
    CHECK(room.inputs[2].receive(kHostConnection, packet(original)) == Admission::WrongPeer);
    CHECK(room.inputs[3].receive(kHostConnection, packet(original)) == Admission::Accepted);
    CHECK(room.inputs[3].receive(102, packet(room.commands(1, 1))) == Admission::WrongPeer);
}

TEST_CASE("lockstep local sampling never consumes a partial malformed or wrongly keyed frame",
          "[lockstep-inputs]") {
    Room room({1, 2, 2, 0});
    auto& guest = room.inputs[2];
    const auto original = room.commands(2, 1, true);
    CHECK_FALSE(guest.sample({}));
    CHECK_FALSE(guest.sample(std::span{original}.first(1)));
    auto changed = original;
    changed.back().magnitude = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(guest.sample(changed));
    changed = original;
    ++changed.front().tick;
    CHECK_FALSE(guest.sample(changed));
    CHECK_FALSE(guest.sample(room.commands(1, 1)));
    CHECK(guest.sampleTick() == 1);
    CHECK(guest.queuedPackets() == 0);
    REQUIRE(guest.sample(original));
    CHECK(guest.sampleTick() == 2);
    CHECK(guest.queuedPackets() == 1);
}

TEST_CASE("lockstep duplicates do not relay twice and conflicting exact bytes retain first input",
          "[lockstep-inputs]") {
    Room room({1, 2, 3, 0});
    auto& host = room.inputs[1];
    const auto original = room.commands(2, 1);
    REQUIRE(host.receive(102, packet(original)) == Admission::Accepted);
    CHECK(host.receive(102, packet(original)) == Admission::Duplicate);
    CHECK(host.queuedPackets() == 1);
    auto changed = original;
    changed.front().direction.x = -0.0f;
    REQUIRE(packet(changed) != packet(original));
    CHECK(host.receive(102, packet(changed)) == Admission::Conflict);
    changed = original;
    changed.front().pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    CHECK(host.receive(102, packet(changed)) == Admission::Conflict);
    CHECK(host.queuedPackets() == 1);
    REQUIRE(host.sample(room.commands(1, 1)));
    REQUIRE(host.receive(103, packet(room.commands(3, 1))) == Admission::Accepted);
    const auto executed = host.advance();
    REQUIRE(executed);
    CHECK(sameCommand((*executed)[1], original.front()));
    CHECK(host.receive(102, packet(original)) == Admission::Duplicate);
    CHECK(host.receive(102, packet(changed)) == Admission::Conflict);
}

TEST_CASE("lockstep future and consumed history windows stay bounded", "[lockstep-inputs]") {
    Room room;
    auto& host = room.inputs[1];
    for (u64 tick = 1; tick <= LockstepInputs::kWindow + 1; ++tick) {
        REQUIRE(host.sample(room.commands(1, tick)));
        REQUIRE(host.receive(102, packet(room.commands(2, tick))) == Admission::Accepted);
        REQUIRE(host.advance());
        REQUIRE(host.flush(room.wires[1]));
        room.wires[1].sent.clear();
    }
    CHECK(host.receive(102, packet(room.commands(2, 1))) == Admission::Stale);
    CHECK(host.receive(102, packet(room.commands(2, 2))) == Admission::Duplicate);
    CHECK(host.receive(102, packet(room.commands(2, host.tick() + LockstepInputs::kWindow))) ==
          Admission::TooEarly);
    CHECK(host.receive(102, packet(room.commands(2, std::numeric_limits<u64>::max()))) ==
          Admission::TooEarly);
    CHECK(host.receive(102, packet(room.commands(2, host.tick() + LockstepInputs::kWindow - 1))) ==
          Admission::Accepted);
    CHECK_FALSE(host.ready());
}

TEST_CASE("lockstep congestion retries the exact reliable head and never resamples an edge",
          "[lockstep-inputs]") {
    Room room({1, 2, 0, 0}, 2);
    auto& guest = room.inputs[2];
    auto& wire = room.wires[2];
    const auto first = room.commands(2, 3, true);
    REQUIRE(guest.sample(first));
    REQUIRE(guest.advance()); // agreed neutral tick1 only
    const auto second = room.commands(2, 4, true);
    REQUIRE(guest.sample(second));
    REQUIRE(guest.queuedPackets() == 2);
    wire.result = PacketTransport::SendResult::Congested;
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        REQUIRE(guest.flush(wire));
        CHECK_FALSE(guest.failed());
        CHECK(wire.sent.empty());
        CHECK(guest.queuedPackets() == 2);
        CHECK(guest.sampleTick() == 5);
    }
    wire.result = PacketTransport::SendResult::Sent;
    wire.budget = 1;
    REQUIRE(guest.flush(wire));
    REQUIRE(wire.sent.size() == 1);
    CHECK(wire.sent[0].bytes == packet(first));
    CHECK(wire.sent[0].delivery == PacketTransport::Delivery::Reliable);
    CHECK(guest.queuedPackets() == 1);
    wire.budget = 1;
    REQUIRE(guest.flush(wire));
    REQUIRE(wire.sent.size() == 2);
    CHECK(wire.sent[1].bytes == packet(second));
    CHECK(guest.queuedPackets() == 0);
    REQUIRE(guest.flush(wire));
    CHECK(wire.sent.size() == 2);
}

TEST_CASE("lockstep full relay queues refuse atomically and accept the exact retried packet",
          "[lockstep-inputs]") {
    static_assert(LockstepInputs::kQueuedPackets > LockstepInputs::kWindow + 1);
    Room room({1, 2, 3, 4});
    auto& host = room.inputs[1];
    REQUIRE(host.sample(room.commands(1, 1)));
    for (u64 tick = 1; tick <= LockstepInputs::kWindow; ++tick) {
        REQUIRE(host.receive(102, packet(room.commands(2, tick))) == Admission::Accepted);
    }
    constexpr u64 kRemaining = LockstepInputs::kQueuedPackets - LockstepInputs::kWindow - 1;
    for (u64 tick = 1; tick <= kRemaining; ++tick) {
        REQUIRE(host.receive(103, packet(room.commands(3, tick))) == Admission::Accepted);
    }
    const auto pending = packet(room.commands(3, kRemaining + 1, true));
    const auto queued = host.queuedPackets();
    CHECK(queued <= 3 * LockstepInputs::kQueuedPackets);
    CHECK(host.receive(103, pending) == Admission::Full);
    CHECK(host.receive(103, pending) == Admission::Full); // not partially admitted/duplicate
    CHECK(host.queuedPackets() == queued);
    CHECK_FALSE(host.advance());
    REQUIRE(host.flush(room.wires[1]));
    CHECK(host.queuedPackets() == 0);
    REQUIRE(host.receive(103, pending) == Admission::Accepted);
    CHECK(host.receive(103, pending) == Admission::Duplicate);
    CHECK(host.queuedPackets() == 2);
}

TEST_CASE("lockstep disconnects and permanent send failures stop the epoch without neutral input",
          "[lockstep-inputs]") {
    const u32 failure = GENERATE(0U, 1U, 2U);
    CAPTURE(failure);
    Room room;
    auto& host = room.inputs[1];
    REQUIRE(host.sample(room.commands(1, 1, true)));
    REQUIRE(host.receive(102, packet(room.commands(2, 1, true))) == Admission::Accepted);
    REQUIRE(host.ready());
    host.disconnected(0);
    host.disconnected(999);
    REQUIRE_FALSE(host.failed());
    if (failure == 0) {
        host.disconnected(102);
    } else {
        room.wires[1].result = failure == 1 ? PacketTransport::SendResult::Disconnected
                                            : PacketTransport::SendResult::Invalid;
        CHECK_FALSE(host.flush(room.wires[1]));
    }
    CHECK(host.failed());
    CHECK_FALSE(host.ready());
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.sampleReady());
    CHECK_FALSE(host.sample(room.commands(1, 2)));
    CHECK_FALSE(host.flush(room.wires[1]));
    CHECK(host.receive(102, packet(room.commands(2, 2))) == Admission::WrongPeer);
    CHECK(host.tick() == 1);
    CHECK_FALSE(host.open(1, kEpoch + 1, room.owners, room.grants, 0,
                          std::array{LockstepInputs::Link{2, 102}}));
}
