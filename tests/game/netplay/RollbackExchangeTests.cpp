#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestSupport.h"
#include "fixtures/PacketStar.h"
#include "fixtures/RecordingPacketTransport.h"
#include "game/netplay/RollbackExchange.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Exchange = RollbackExchange;
constexpr u64 kEpoch = 63;
constexpr PacketTransport::Connection kHost = 80;
constexpr test::StarLinks kStar{kHost};

/** A recording wire that checks every frame and ACK goes unreliably and fits one fragment. */
struct Wire final : test::RecordingPacketTransport {
private:
    void inspect(Connection /*connection*/, std::span<const u8> bytes, Delivery delivery) override {
        CHECK(delivery == Delivery::Unreliable);
        CHECK(bytes.size() <= Exchange::kMaxBytes);
        CHECK(bytes.size() + 1 <= 1000); // one provider fragment, including room envelope
    }
};

std::vector<u8> encode(const Exchange::Message& message) {
    auto bytes = Exchange::encode(message);
    REQUIRE(bytes);
    return *bytes;
}

struct Room : test::PacketStar<Wire> {
    Exchange::Grants grants{};
    std::array<Exchange, kSlots> peers;
    std::array<std::array<std::map<u64, std::vector<u8>>, kSeats>, kSlots> actual;
    usize sends = 0;
    usize dropped = 0;
    u64 nowUs = 0;
    bool reject = false;
    std::function<bool(u8, u8, const Exchange::Message&)> drop;
    explicit Room(Exchange::Owners roster = {1, 2, 3, 4}, u8 delay = 0)
        : PacketStar(roster, kStar) {
        for (usize seat = 0; seat < owners.size(); ++seat) {
            grants[seat] = (owners[seat] != 0u) ? static_cast<u32>(20 + seat) : 0;
        }
        std::vector<Exchange::Link> guests;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                guests.push_back({peer, kStar.guest(peer)});
                const std::array host{Exchange::Link{1, kHost}};
                REQUIRE(peers[peer].open(peer, kEpoch, owners, grants, delay, host, [this, peer] {
                    return nowUs + static_cast<u64>(100'000'000) * peer;
                }));
            }
        }
        REQUIRE(peers[1].open(1, kEpoch, owners, grants, delay, guests,
                              [this] { return nowUs + 100'000'000; }));
    }
    std::vector<InputCommand> frame(u8 peer, u64 tick) const {
        std::vector<InputCommand> result;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] == peer) {
                InputCommand command;
                command.epoch = kEpoch;
                command.tick = tick;
                command.seat = seat;
                command.grant = grants[seat];
                command.pressedButtons = tick % 3 == 0 ? static_cast<u32>(CommandPress::Attack) : 0;
                result.push_back(command);
            }
        }
        return result;
    }
    bool consume(u8 peer, const Exchange::Message& message) {
        if (reject) {
            return false;
        }
        for (const auto& command : message.commands) {
            const auto bytes = InputPacket::encode(std::span{&command, 1});
            REQUIRE(bytes);
            auto& history = actual[peer][command.seat];
            const auto [found, added] = history.emplace(command.tick, *bytes);
            CHECK((added || found->second == *bytes));
        }
        return true;
    }
    /** What a delivered frame or ACK meets: its decoding, the case's drop, then its receiver. */
    auto arrival() {
        return [this](u8 source, u8 target, PacketTransport::Connection connection,
                      const Packet& packet) {
            const auto decoded = Exchange::decode(packet.bytes);
            REQUIRE(decoded);
            ++sends;
            if (drop && drop(source, target, *decoded)) {
                ++dropped;
                return;
            }
            const auto result =
                peers[target].receive(connection, packet.bytes, 0,
                                      [&](const auto& m, auto) { return consume(target, m); });
            REQUIRE(result ==
                    (reject ? Exchange::Admission::Rejected : Exchange::Admission::Accepted));
        };
    }
    /** Hands on what `source` sent, newest first, to exercise reordered frames and ACKs. */
    void deliver(u8 source) { PacketStar::deliver(source, arrival(), Order::NewestFirst); }
    void pump() {
        // Relay before doing any simulation, not on its next native tick.
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(peers[peer].flush(wires[peer]));
                deliver(peer);
            }
        }
        REQUIRE(peers[1].flush(wires[1]));
        deliver(1);
    }
};
} // namespace

TEST_CASE("console input bursts split by encoded size without losing complete seat frames",
          "[rollback-exchange][console-input]") {
    Room room({1, 1, 1, 2});
    std::vector<InputCommand> commands;
    for (u64 tick = 1; tick <= 8; ++tick) {
        auto frame = room.frame(1, tick);
        for (auto& command : frame) {
            command.console = ConsoleAction{3, "spawn " + std::string(154, 'a')};
        }
        commands.insert(commands.end(), frame.begin(), frame.end());
    }
    REQUIRE(room.peers[1].publish(commands, 1));
    REQUIRE(room.peers[1].flush(room.wires[1]));
    CHECK(room.wires[1].sent.size() >= 8);
    for (const auto& packet : room.wires[1].sent) {
        const auto decoded = Exchange::decode(packet.bytes);
        REQUIRE(decoded);
        CHECK(decoded->commands.size() == 3);
        CHECK(packet.bytes.size() <= Exchange::kMaxBytes);
    }
    room.deliver(1);
    for (usize seat = 0; seat < 3; ++seat) {
        CHECK(room.actual[2][seat].size() == 8);
    }
}

TEST_CASE("a maximum four-seat console frame still fits the single-fragment envelope",
          "[rollback-exchange][console-input]") {
    const Room room({1, 1, 1, 1});
    auto frame = room.frame(1, 1);
    for (auto& command : frame) {
        command.console = ConsoleAction{3, "spawn " + std::string(154, 'a')};
    }
    const auto packet = Exchange::encode({kEpoch, 1, 1, {}, frame});
    REQUIRE(packet);
    CHECK(packet->size() + 1 <= 1000);
    const auto decoded = Exchange::decode(*packet);
    REQUIRE(decoded);
    CHECK(decoded->commands.size() == 4);
    CHECK(InputPacket::encode(decoded->commands) == InputPacket::encode(frame));
}

TEST_CASE("direct guest links cannot smuggle host-only console input",
          "[rollback-exchange][console-input]") {
    constexpr Exchange::Owners kOwners{1, 2, 3, 0};
    constexpr Exchange::Grants kGrants{10, 11, 12, 0};
    Exchange guest;
    const std::array links{Exchange::Link{1, kHost}, Exchange::Link{3, 23}};
    REQUIRE(guest.open(2, kEpoch, kOwners, kGrants, 0, links));
    InputCommand command;
    command.epoch = kEpoch;
    command.tick = 1;
    command.grant = 12;
    command.seat = 2;
    command.console = ConsoleAction{2, "god"};
    Exchange::Message message;
    message.epoch = kEpoch;
    message.origin = 3;
    message.commands = {command};
    const auto bytes = Exchange::encode(message);
    REQUIRE(bytes);
    bool consumed = false;
    CHECK(guest.receive(23, *bytes, 0, [&](const auto&, auto) {
        consumed = true;
        return true;
    }) == Exchange::Admission::Invalid);
    CHECK_FALSE(consumed);
}

TEST_CASE("rollback datagrams bound and authenticate their envelope before parsing input",
          "[rollback-exchange]") {
    const Room room;
    Exchange::Message message{kEpoch, 2, 3, {}, room.frame(2, 3)};
    const auto bytes = encode(message);
    CHECK(bytes.size() ==
          Exchange::kHeaderBytes + InputPacket::kHeaderBytes + InputPacket::kCommandBytes);
    CHECK(Exchange::recognizes(bytes));
    REQUIRE(Exchange::decode(bytes));
    for (usize size = 0; size < bytes.size(); ++size) {
        // Header alone is a valid empty heartbeat.
        CHECK(Exchange::decode(std::span{bytes}.first(size)).has_value() ==
              (size == Exchange::kHeaderBytes));
    }
    for (const usize offset : {usize{0}, usize{4}, usize{6}, usize{7}}) {
        auto bad = bytes;
        bad[offset] ^= 0x80;
        CHECK_FALSE(Exchange::decode(bad));
    }
    message.origin = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.origin = 2;
    message.epoch = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.epoch = kEpoch;
    message.commands[0].epoch++;
    CHECK_FALSE(Exchange::encode(message));
    message.commands[0].epoch = message.epoch;
    message.progressSerial = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.progressSerial = std::numeric_limits<u64>::max();
    message.progressAgeUs = Exchange::kMaxProgressAgeUs;
    REQUIRE(Exchange::decode(encode(message)));
    ++message.progressAgeUs;
    CHECK_FALSE(Exchange::encode(message));
    CHECK_FALSE(Exchange::decode(std::vector<u8>(Exchange::kMaxBytes + 1)));
}

TEST_CASE("rollback datagrams keep their byte layout", "[rollback-exchange]") {
    Exchange::Message message;
    message.epoch = 0x1122334455667788ULL;
    message.origin = 3;
    message.nextTick = 0x0102030405060708ULL;
    message.received = {0, 0x1112131415161718ULL, 2, 0x2122232425262728ULL, 4};
    message.progressSerial = 0x3132333435363738ULL;
    message.progressAgeUs = 0x00012345;
    const auto heartbeat = encode(message);
    constexpr std::string_view kHeartbeat = "47445249"         // magic
                                            "02030000"         // version, origin, zero
                                            "8877665544332211" // epoch
                                            "0807060504030201" // next tick
                                            "1817161514131211" // received from peer one
                                            "0200000000000000" // peer two
                                            "2827262524232221" // peer three
                                            "0400000000000000" // peer four
                                            "3837363534333231" // progress serial
                                            "45230100";        // progress age
    CHECK(test::hexBytes(heartbeat) == kHeartbeat);
    InputCommand command;
    command.epoch = message.epoch;
    command.tick = 9;
    command.grant = 1;
    command.seat = 2;
    message.commands = {command};
    const auto bytes = encode(message);
    const auto inputs = InputPacket::encode(message.commands);
    REQUIRE(inputs);
    auto expected = heartbeat;
    expected.insert(expected.end(), inputs->begin(), inputs->end());
    CHECK(bytes == expected);
    const auto decoded = Exchange::decode(bytes);
    REQUIRE(decoded);
    CHECK(decoded->received == message.received);
    CHECK(decoded->progressSerial == message.progressSerial);
    CHECK(Exchange::encode(*decoded) == bytes);
}

TEST_CASE("rollback exchange rejects unadmitted origins and does not ACK refused frames",
          "[rollback-exchange]") {
    Room room;
    Exchange::Message message{kEpoch, 2, 1, {}, room.frame(2, 1)};
    usize calls = 0;
    const auto consume = [&](const auto&, auto) {
        ++calls;
        return false;
    };
    CHECK(room.peers[1].receive(103, encode(message), 0, consume) ==
          Exchange::Admission::WrongPeer);
    CHECK(room.peers[3].receive(999, encode(message), 0, consume) ==
          Exchange::Admission::WrongPeer);
    CHECK(calls == 0);
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Rejected);
    CHECK(calls == 1);
    CHECK(room.peers[1].receivedThrough(2) == 0);
    CHECK(room.peers[1].retainedFrames(2) == 0);
    message.received[1] = 1; // cannot ACK a host input that has never been sampled
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Invalid);
    CHECK(calls == 1);
    message.received[1] = 0;
    message.received[3] = 1; // a third party's input may have come on a direct link first
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Rejected);
    CHECK(calls == 2);
    message.received[3] = 0;
    message.commands[0].grant++;
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Invalid);
    CHECK(calls == 2);
    message.commands[0].grant--;
    message.epoch++;
    message.commands[0].epoch++;
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) ==
          Exchange::Admission::WrongEpoch);
}

TEST_CASE("eight frames of three local seats split into complete bounded datagrams",
          "[rollback-exchange]") {
    Room room({1, 2, 2, 2});
    for (u64 tick = 1; tick <= 8; ++tick) {
        REQUIRE(room.peers[2].publish(room.frame(2, tick), tick));
    }
    REQUIRE(room.peers[2].flush(room.wires[2]));
    REQUIRE(room.wires[2].sent.size() == 2);
    usize total = 0;
    for (const auto& packet : room.wires[2].sent) {
        const auto message = Exchange::decode(packet.bytes);
        REQUIRE(message);
        CHECK(message->commands.size() % 3 == 0);
        total += message->commands.size();
    }
    CHECK(total == 24);
    room.deliver(2);
    CHECK(room.peers[1].receivedThrough(2) == 8);
    for (usize seat = 1; seat < 4; ++seat) {
        CHECK(room.actual[1][seat].size() == 8);
    }
    Exchange::Message incomplete{kEpoch, 2, 9, {}, room.frame(2, 9)};
    incomplete.commands.pop_back();
    CHECK(room.peers[1].receive(102, encode(incomplete), 0, [](const auto&, auto) {
        return true;
    }) == Exchange::Admission::Invalid);
    CHECK(room.peers[1].receivedThrough(2) == 8);
}

TEST_CASE("end to end ACK recovers an old guest input after the relay already received it",
          "[rollback-exchange]") {
    Room room({1, 2, 3, 0});
    bool holdFirst = true;
    room.drop = [&](u8 from, u8 to, const auto& message) {
        return holdFirst && from == 1 && to == 3 && message.origin == 2 &&
               std::ranges::any_of(message.commands,
                                   [](const auto& input) { return input.tick == 1; });
    };
    for (u64 tick = 1; tick <= 12; ++tick) {
        for (u8 peer = 1; peer <= 3; ++peer) {
            REQUIRE(room.peers[peer].publish(room.frame(peer, tick), tick));
        }
        room.pump();
    }
    REQUIRE(room.dropped > 0);
    CHECK(room.peers[1].receivedThrough(2) == 12);
    CHECK(room.peers[3].receivedThrough(2) == 0);
    CHECK(room.peers[2].acknowledgedThrough(1, 2) > 0);
    CHECK(room.peers[2].acknowledgedThrough(3, 2) == 0);
    CHECK(room.peers[2].retainedFrames(2) == 12);
    // No new samples or simulation: the stalled link must still recover.
    holdFirst = false;
    for (usize pass = 0; pass < 5; ++pass) {
        room.pump();
    }
    CHECK(room.peers[3].receivedThrough(2) == 12);
    CHECK(room.peers[2].acknowledgedThrough(3, 2) == 12);
    CHECK(room.peers[2].retainedFrames(2) == 8);
    for (u64 tick = 1; tick <= 12; ++tick) {
        CHECK(room.actual[3][1].contains(tick));
    }
}

TEST_CASE("rollback delivery tolerates reordered lossy traffic and bounds retained history",
          "[rollback-exchange]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    Room room({1, 2, 3, 4}, delay);
    room.drop = [&](u8, u8, const auto&) { return room.sends % 50 == 0; };
    for (u64 tick = 1 + delay; tick <= 240; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            REQUIRE(room.peers[peer].publish(room.frame(peer, tick), tick - delay));
        }
        room.pump();
    }
    room.drop = {};
    for (usize pass = 0; pass < 5; ++pass) {
        room.pump();
    }
    CHECK(room.dropped > 0);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.peers[peer].failure() == Exchange::Failure::None);
        for (u8 origin = 1; origin <= 4; ++origin) {
            CHECK(room.peers[peer].receivedThrough(origin) == 240);
            CHECK(room.peers[peer].retainedFrames(origin) == 8);
            if (peer != origin) {
                CHECK(room.actual[peer][origin - 1].size() == 240 - delay);
            }
        }
    }
}

TEST_CASE("rollback congestion retains input without an expanding packet queue",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.peers[2].publish(room.frame(2, 1), 1));
    room.wires[2].result = PacketTransport::SendResult::Congested;
    for (usize retry = 0; retry < 150; ++retry) {
        REQUIRE(room.peers[2].flush(room.wires[2]));
    }
    CHECK(room.wires[2].sent.empty());
    CHECK(room.peers[2].retainedFrames(2) == 1);
    room.wires[2].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(room.peers[1].receivedThrough(2) == 1);
    room.peers[2].disconnected(999);
    CHECK(room.peers[2].failure() == Exchange::Failure::None);
    room.peers[2].disconnected(kHost);
    CHECK(room.peers[2].failure() == Exchange::Failure::Disconnected);
    CHECK_FALSE(room.peers[2].flush(room.wires[2]));
}

TEST_CASE("conflicting actual input stops the epoch without overwriting accepted commands",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.peers[2].publish(room.frame(2, 1), 1));
    room.pump();
    Exchange::Message bad{kEpoch, 2, 1, {}, room.frame(2, 1)};
    bad.commands[0].direction.x = -0.0f;
    CHECK(room.peers[1].receive(102, encode(bad), 0, [](const auto&, auto) { return true; }) ==
          Exchange::Admission::Conflict);
    CHECK(room.peers[1].failure() == Exchange::Failure::Conflict);
    CHECK(room.peers[1].receivedThrough(2) == 1);
    CHECK(room.peers[1].retainedFrames(2) == 1);

    auto localConflict = room.frame(2, 1);
    localConflict[0].pressedButtons = static_cast<u32>(CommandPress::Attack);
    CHECK_FALSE(room.peers[2].publish(localConflict, 1));
    CHECK(room.peers[2].failure() == Exchange::Failure::Conflict);
    CHECK_FALSE(room.peers[2].flush(room.wires[2]));
}

TEST_CASE("a one datagram budget cannot starve later routed origins", "[rollback-exchange]") {
    Room room;
    for (u8 peer = 1; peer <= 4; ++peer) {
        REQUIRE(room.peers[peer].publish(room.frame(peer, 1), 1));
    }
    for (usize pass = 0; pass < 12; ++pass) {
        for (u8 destination = 2; destination <= 4; ++destination) {
            room.wires[1].budgets[100 + destination] = 1;
        }
        room.pump();
    }
    for (u8 receiver = 1; receiver <= 4; ++receiver) {
        for (u8 origin = 1; origin <= 4; ++origin) {
            CHECK(room.peers[receiver].receivedThrough(origin) == 1);
            // What the origin acknowledged of this peer:
            // NOLINTNEXTLINE(readability-suspicious-call-argument)
            CHECK(room.peers[receiver].acknowledgedThrough(origin, receiver) == 1);
        }
    }
}

TEST_CASE("an unresponsive receiver cannot cause unbounded input retention",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    for (u64 tick = 1; tick <= Exchange::kHistory; ++tick) {
        REQUIRE(room.peers[1].publish(room.frame(1, tick), tick));
    }
    CHECK(room.peers[1].retainedFrames(1) == Exchange::kHistory);
    CHECK_FALSE(
        room.peers[1].publish(room.frame(1, Exchange::kHistory + 1), Exchange::kHistory + 1));
    CHECK(room.peers[1].failure() == Exchange::Failure::Window);
    CHECK(room.peers[1].retainedFrames(1) == Exchange::kHistory);
    CHECK_FALSE(room.peers[1].flush(room.wires[1]));
}

TEST_CASE("relayed progress carries upstream age and residence without refreshing cached reports",
          "[rollback-exchange][rollback-pacing]") {
    Room room({1, 2, 3, 0});
    Exchange::Message source{kEpoch, 2, 10, {}, {}, 7, 0};
    room.nowUs = 80'000;
    std::optional<u8> age;
    const auto accept = [&](const auto&, std::optional<u8> transit) {
        age = transit;
        return true;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 160, accept) ==
            Exchange::Admission::Accepted);
    REQUIRE(age == 5); // first 80 ms hop
    const auto forwarded = [&](u8 origin) {
        REQUIRE(room.peers[1].flush(room.wires[1]));
        std::vector<u8> bytes;
        for (const auto& packet : room.wires[1].sent) {
            const auto message = Exchange::decode(packet.bytes);
            REQUIRE(message);
            if (packet.connection == 103 && message->origin == origin) {
                bytes = packet.bytes;
            }
        }
        room.wires[1].sent.clear();
        REQUIRE_FALSE(bytes.empty());
        return bytes;
    };
    room.nowUs = 100'000; // held at relay for 20 ms
    const auto first = forwarded(2);
    REQUIRE(Exchange::decode(first)->progressAgeUs == 100'000);
    room.nowUs = 110'000;
    REQUIRE(room.peers[3].receive(kHost, first, 20, accept) == Exchange::Admission::Accepted);
    CHECK(age == 7); // 80 + 20 + 10 ms, not the final 10 ms alone

    room.nowUs = 1'000'000; // an upstream outage; relay keeps servicing traffic
    const auto cached = forwarded(2);
    REQUIRE(Exchange::decode(cached)->progressAgeUs == Exchange::kMaxProgressAgeUs);
    REQUIRE(room.peers[3].receive(kHost, cached, 20, accept) == Exchange::Admission::Accepted);
    CHECK_FALSE(age); // a larger age on the same report is not new peer progress
    REQUIRE(room.peers[3].receive(kHost, first, 20, accept) == Exchange::Admission::Accepted);
    CHECK_FALSE(age); // nor is reordered delivery of the earlier copy

    // The source is actually paused but still publishes a new heartbeat.
    ++source.progressSerial;
    REQUIRE(room.peers[1].receive(102, encode(source), 160, accept) ==
            Exchange::Admission::Accepted);
    CHECK(age == 5);
    const auto heartbeat = forwarded(2);
    room.nowUs += 10'000;
    REQUIRE(room.peers[3].receive(kHost, heartbeat, 20, accept) == Exchange::Admission::Accepted);
    CHECK(age == 6); // fresh same-head report, measured path is 90 ms again

    REQUIRE(room.peers[1].publish({}, 10));
    const auto host = forwarded(1);
    REQUIRE(Exchange::decode(host)->progressAgeUs == 0);
    room.nowUs += 10'000;
    REQUIRE(room.peers[3].receive(kHost, host, 20, accept) == Exchange::Admission::Accepted);
    CHECK(age == 1); // peer one has no upstream hop
}

TEST_CASE("progress admission is transactional and older reports still recover actual inputs",
          "[rollback-exchange][rollback-pacing]") {
    Room room({1, 2, 3, 0});
    Exchange::Message source{kEpoch, 2, 10, {}, room.frame(2, 1), 20, 0};
    usize fresh = 0;
    const auto reject = [&](const auto&, std::optional<u8> age) {
        REQUIRE(age);
        ++fresh;
        return false;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 20, reject) ==
            Exchange::Admission::Rejected);
    CHECK(room.peers[1].receivedThrough(2) == 0);
    REQUIRE(room.peers[1].flush(room.wires[1]));
    for (const auto& packet : room.wires[1].sent) {
        REQUIRE(Exchange::decode(packet.bytes));
        CHECK(Exchange::decode(packet.bytes)->origin != 2);
    }
    std::optional<u8> last;
    const auto accept = [&](const auto&, std::optional<u8> age) {
        last = age;
        fresh += age.has_value();
        return true;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 20, accept) ==
            Exchange::Admission::Accepted);
    CHECK(fresh == 2); // refusal did not consume the report serial
    CHECK(last == 1);
    CHECK(room.peers[1].receivedThrough(2) == 1);

    source.progressSerial = 19;
    source.nextTick = 9;
    source.commands = room.frame(2, 2);
    REQUIRE(room.peers[1].receive(102, encode(source), 20, accept) ==
            Exchange::Admission::Accepted);
    CHECK_FALSE(last);
    CHECK(room.peers[1].receivedThrough(2) == 2);
    source.progressSerial = 21; // a new serial cannot move the head backwards
    CHECK(room.peers[1].receive(102, encode(source), 20, accept) == Exchange::Admission::Invalid);
    CHECK(fresh == 2);
    source.progressSerial = 20; // the old serial cannot describe a different head
    CHECK(room.peers[1].receive(102, encode(source), 20, accept) == Exchange::Admission::Conflict);
    CHECK(room.peers[1].failure() == Exchange::Failure::Conflict);
}

TEST_CASE("progress ages saturate and unknown RTT remains a bounded pacing estimate",
          "[rollback-exchange][rollback-pacing]") {
    const s32 ping = GENERATE(s32{-1}, std::numeric_limits<s32>::max());
    Room room({1, 2, 0, 0});
    const Exchange::Message message{kEpoch, 2, 1, {}, {}, 1, 0};
    REQUIRE(
        room.peers[1].receive(102, encode(message), ping, [](const auto&, std::optional<u8> age) {
            REQUIRE(age == RollbackInputs::kPredictionTicks);
            return true;
        }) == Exchange::Admission::Accepted);
    room.nowUs = std::numeric_limits<u64>::max() - 500'000'000;
    REQUIRE(room.peers[1].flush(room.wires[1]));
    REQUIRE_FALSE(room.wires[1].sent.empty());
    for (const auto& packet : room.wires[1].sent) {
        const auto decoded = Exchange::decode(packet.bytes);
        REQUIRE(decoded);
        CHECK(decoded->progressAgeUs == Exchange::kMaxProgressAgeUs);
    }
    room.nowUs = 0; // bad injected clocks must not unsigned-underflow residence
    CHECK_FALSE(room.peers[1].flush(room.wires[1]));
    CHECK(room.peers[1].failure() == Exchange::Failure::Transport);
}

TEST_CASE("asymmetric star progress paces an actually faster peer without slowing equal peers",
          "[rollback-exchange][rollback-pacing]") {
    const usize count = GENERATE(usize{3}, usize{4});
    Room room(count == 3 ? Exchange::Owners{1, 2, 3, 0} : Exchange::Owners{1, 2, 3, 4});
    std::array<RollbackInputs, 5> inputs;
    std::array<usize, 5> idles{};
    // Guest A is 80 ms from the host, B 10 ms, C 30 ms. The receiver only
    // supplies its direct link RTT; no test code adds an upstream hop.
    constexpr std::array<u64, 5> kHopUs{0, 0, 80'000, 10'000, 30'000};
    struct Pending {
        u8 sender, receiver;
        u64 arrives;
        std::vector<u8> bytes;
    };
    std::vector<Pending> pending;
    for (u8 peer = 1; peer <= count; ++peer) {
        REQUIRE(inputs[peer].open(peer, kEpoch, room.owners, room.grants, 0));
    }
    for (u64 wall = 0; wall < 240; ++wall) {
        room.nowUs = wall * 1'000'000 / 60;
        std::ranges::sort(pending, {}, &Pending::arrives);
        usize delivered = 0;
        while (delivered < pending.size() && pending[delivered].arrives <= room.nowUs) {
            const auto& packet = pending[delivered++];
            const u8 guest = packet.sender == 1 ? packet.receiver : packet.sender;
            const auto connection = packet.sender == 1 ? kHost : 100 + packet.sender;
            REQUIRE(room.peers[packet.receiver].receive(
                        connection, packet.bytes, static_cast<s32>(kHopUs[guest] / 500),
                        [&](const Exchange::Message& message, std::optional<u8> age) {
                            if (!message.commands.empty()) {
                                const auto admitted = inputs[packet.receiver].submit(
                                    message.origin, message.commands);
                                REQUIRE((admitted == RollbackInputs::Admission::Accepted ||
                                         admitted == RollbackInputs::Admission::Duplicate ||
                                         admitted == RollbackInputs::Admission::Stale));
                            }
                            if (age) {
                                const auto progress = inputs[packet.receiver].observePeerTick(
                                    message.origin, message.epoch, message.nextTick, *age);
                                REQUIRE((progress == RollbackInputs::Admission::Accepted ||
                                         progress == RollbackInputs::Admission::Duplicate));
                            }
                            return true;
                        }) == Exchange::Admission::Accepted);
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(delivered));
        for (u8 peer = 1; peer <= count; ++peer) {
            // Only after the equal-rate control, run the host's scheduler twice
            // as fast. Other peers retain their own simulation and 60 Hz cadence.
            const usize budget = wall >= 180 && peer == 1 ? 2 : 1;
            for (usize step = 0; step < budget; ++step) {
                if (inputs[peer].sampleReady()) {
                    auto frame = room.frame(peer, inputs[peer].sampleTick());
                    for (auto& command : frame) {
                        command.pressedButtons = 0; // neutral prediction needs no rewind
                    }
                    REQUIRE(inputs[peer].sample(frame));
                }
                if (inputs[peer].timeSyncIdle()) {
                    ++idles[peer];
                } else if (inputs[peer].ready()) {
                    REQUIRE(inputs[peer].advance());
                }
                CHECK_FALSE(inputs[peer].mismatchTick());
                CHECK(inputs[peer].tick() - 1 - inputs[peer].confirmedTick() <=
                      RollbackInputs::kPredictionTicks);
            }
            REQUIRE(room.peers[peer].publish(inputs[peer].redundantInputs(), inputs[peer].tick()));
        }
        if (wall == 179) {
            for (u8 peer = 1; peer <= count; ++peer) {
                CAPTURE(count, peer);
                CHECK(idles[peer] == 0);
                CHECK(inputs[peer].tick() == 181);
            }
        }
        for (u8 peer = 1; peer <= count; ++peer) {
            REQUIRE(room.peers[peer].flush(room.wires[peer]));
            for (auto& packet : room.wires[peer].sent) {
                const u8 receiver = peer == 1 ? static_cast<u8>(packet.connection - 100) : 1;
                const u8 guest = peer == 1 ? receiver : peer;
                pending.push_back(
                    {peer, receiver, room.nowUs + kHopUs[guest], std::move(packet.bytes)});
            }
            room.wires[peer].sent.clear();
        }
    }
    CHECK(idles[1] > 0);
    for (u8 peer = 1; peer <= count; ++peer) {
        CHECK(inputs[peer].tick() > 220);
        CHECK(room.peers[peer].failure() == Exchange::Failure::None);
    }
}

TEST_CASE("a guest's direct link carries that guest's own inputs sooner and may go away",
          "[rollback-exchange][direct-link]") {
    const Exchange::Owners owners{1, 2, 3, 0};
    Exchange::Grants grants{};
    for (usize seat = 0; seat < owners.size(); ++seat) {
        grants[seat] = owners[seat] != 0 ? static_cast<u32>(20 + seat) : 0;
    }
    const auto clock = [] { return u64{1'000'000}; };
    // The host has nothing to add; a guest's extra link names another guest, once, on
    // its own connection.
    const std::array hostExtra{Exchange::Link{2, 102}, Exchange::Link{3, 103},
                               Exchange::Link{4, 104}};
    CHECK_FALSE(Exchange{}.open(1, kEpoch, owners, grants, 0, hostExtra, clock));
    const std::array absent{Exchange::Link{1, kHost}, Exchange::Link{4, 304}};
    CHECK_FALSE(Exchange{}.open(3, kEpoch, owners, grants, 0, absent, clock));
    const std::array shared{Exchange::Link{1, kHost}, Exchange::Link{2, kHost}};
    CHECK_FALSE(Exchange{}.open(3, kEpoch, owners, grants, 0, shared, clock));
    Exchange second;
    Exchange third;
    const std::array secondLinks{Exchange::Link{1, kHost}, Exchange::Link{3, 203}};
    const std::array thirdLinks{Exchange::Link{1, kHost}, Exchange::Link{2, 302}};
    REQUIRE(second.open(2, kEpoch, owners, grants, 0, secondLinks, clock));
    REQUIRE(third.open(3, kEpoch, owners, grants, 0, thirdLinks, clock));
    // The second guest's own frame goes out on both of its links.
    InputCommand command;
    command.epoch = kEpoch;
    command.tick = 1;
    command.seat = 1;
    command.grant = grants[1];
    command.pressedButtons = static_cast<u32>(CommandPress::Attack);
    REQUIRE(second.publish(std::span{&command, 1}, 2));
    Wire wire;
    REQUIRE(second.flush(wire));
    std::set<PacketTransport::Connection> destinations;
    for (const auto& packet : wire.sent) {
        destinations.insert(packet.connection);
    }
    CHECK(destinations == std::set<PacketTransport::Connection>{kHost, 203});
    const auto direct = std::ranges::find(wire.sent, 203, &Wire::Packet::connection);
    REQUIRE(direct != wire.sent.end());
    // The third guest takes it straight from the second.
    usize consumed = 0;
    const auto consume = [&](const Exchange::Message& message, auto) {
        CHECK(message.origin == 2);
        consumed += message.commands.size();
        return true;
    };
    CHECK(third.receive(302, direct->bytes, 0, consume) == Exchange::Admission::Accepted);
    CHECK(consumed == 1);
    CHECK(third.receivedThrough(2) == 1);
    // A direct peer cannot speak for the host or for anyone else.
    const auto forged = encode({kEpoch, 1, 2, {}, {}, 1});
    CHECK(third.receive(302, forged, 0, consume) == Exchange::Admission::WrongPeer);
    // When the direct link goes, the stream lives on through the host.
    third.detach(302);
    CHECK(third.failure() == Exchange::Failure::None);
    CHECK(third.receive(302, direct->bytes, 0, consume) == Exchange::Admission::WrongPeer);
    Wire thirdWire;
    REQUIRE(third.flush(thirdWire));
    for (const auto& packet : thirdWire.sent) {
        CHECK(packet.connection == kHost);
    }
}

TEST_CASE("a stream opened after a guest was dropped wants no link to it and waits on nobody",
          "[rollback-exchange][drop]") {
    const Exchange::Owners owners{1, 2, 3, 0};
    Exchange::Grants grants{};
    for (usize seat = 0; seat < owners.size(); ++seat) {
        grants[seat] = owners[seat] != 0 ? static_cast<u32>(20 + seat) : 0;
    }
    const auto clock = [] { return u64{1'000'000}; };
    const std::array<u8, 1> secondGuest{2};
    const std::array<u8, 1> hostPeer{1};
    const std::array<u8, 2> stranger{2, 4};
    const std::array third{Exchange::Link{3, 103}};
    const std::array both{Exchange::Link{2, 102}, Exchange::Link{3, 103}};
    const std::array toHost{Exchange::Link{1, kHost}};
    // Without the drop the host's star still wants every guest; with it, never that one.
    CHECK_FALSE(Exchange{}.open(1, kEpoch, owners, grants, 0, third, clock));
    CHECK_FALSE(Exchange{}.open(1, kEpoch, owners, grants, 0, both, clock, secondGuest));
    // Only another seated guest can be the one dropped.
    CHECK_FALSE(Exchange{}.open(1, kEpoch, owners, grants, 0, third, clock, hostPeer));
    CHECK_FALSE(Exchange{}.open(1, kEpoch, owners, grants, 0, third, clock, stranger));
    CHECK_FALSE(Exchange{}.open(2, kEpoch, owners, grants, 0, toHost, clock, secondGuest));
    Exchange host;
    Exchange guest;
    REQUIRE(host.open(1, kEpoch, owners, grants, 0, third, clock, secondGuest));
    REQUIRE(guest.open(3, kEpoch, owners, grants, 0, toHost, clock, secondGuest));
    const auto command = [&](u8 seat, u64 tick) {
        InputCommand input;
        input.epoch = kEpoch;
        input.tick = tick;
        input.seat = seat;
        input.grant = grants[seat];
        return input;
    };
    const auto consume = [](const Exchange::Message&, auto) { return true; };
    // Each side's frames go to the other alone, and each keeps only the redundant tail once
    // the other holds them: neither waits on the dropped guest's acknowledgement.
    for (u64 tick = 1; tick <= 12; ++tick) {
        CAPTURE(tick);
        const auto fromHost = command(0, tick);
        const auto fromGuest = command(2, tick);
        REQUIRE(host.publish(std::span{&fromHost, 1}, tick + 1));
        REQUIRE(guest.publish(std::span{&fromGuest, 1}, tick + 1));
        Wire hostWire;
        Wire guestWire;
        REQUIRE(host.flush(hostWire));
        REQUIRE(guest.flush(guestWire));
        REQUIRE_FALSE(hostWire.sent.empty());
        REQUIRE_FALSE(guestWire.sent.empty());
        for (const auto& packet : hostWire.sent) {
            CHECK(packet.connection == 103);
            CHECK(guest.receive(kHost, packet.bytes, 0, consume) == Exchange::Admission::Accepted);
        }
        for (const auto& packet : guestWire.sent) {
            CHECK(packet.connection == kHost);
            CHECK(host.receive(103, packet.bytes, 0, consume) == Exchange::Admission::Accepted);
        }
    }
    CHECK(guest.receivedThrough(1) == 12);
    CHECK(host.receivedThrough(3) == 12);
    CHECK(host.retainedFrames(1) == RollbackInputs::kRedundantTicks);
    CHECK(guest.retainedFrames(3) == RollbackInputs::kRedundantTicks);
    CHECK(host.failure() == Exchange::Failure::None);
    CHECK(guest.failure() == Exchange::Failure::None);
}

TEST_CASE("a guest knows how far the host holds the input a direct link brings it sooner",
          "[rollback-exchange][direct-link][drop]") {
    const Exchange::Owners owners{1, 2, 3, 0};
    Exchange::Grants grants{};
    for (usize seat = 0; seat < owners.size(); ++seat) {
        grants[seat] = owners[seat] != 0 ? static_cast<u32>(20 + seat) : 0;
    }
    const auto clock = [] { return u64{1'000'000}; };
    const std::array hostLinks{Exchange::Link{2, 102}, Exchange::Link{3, 103}};
    const std::array starLinks{Exchange::Link{1, kHost}};
    const std::array thirdLinks{Exchange::Link{1, kHost}, Exchange::Link{2, 302}};
    Exchange host;
    Exchange star;  // a third guest hearing the second only through the host
    Exchange third; // one hearing it directly as well
    REQUIRE(host.open(1, kEpoch, owners, grants, 0, hostLinks, clock));
    REQUIRE(star.open(3, kEpoch, owners, grants, 0, starLinks, clock));
    REQUIRE(third.open(3, kEpoch, owners, grants, 0, thirdLinks, clock));
    CHECK_FALSE(host.hostReceipt(2));
    CHECK_FALSE(star.hostReceipt(2)); // the host holds all it relays
    CHECK_FALSE(third.hostReceipt(1));
    CHECK_FALSE(third.hostReceipt(3));
    CHECK(third.hostReceipt(2) == 0);
    InputCommand command;
    command.epoch = kEpoch;
    command.tick = 1;
    command.seat = 1;
    command.grant = grants[1];
    const auto consume = [](const Exchange::Message&, auto) { return true; };
    // The second guest's first frame reaches the third directly, before the host has it.
    REQUIRE(third.receive(302, encode({kEpoch, 2, 2, {}, {command}, 1}), 0, consume) ==
            Exchange::Admission::Accepted);
    CHECK(third.receivedThrough(2) == 1);
    CHECK(third.hostReceipt(2) == 0);
    // The host's own acknowledgement says when it holds it too.
    Exchange::Message report{kEpoch, 1, 1, {}, {}, 1};
    report.received[2] = 1;
    REQUIRE(third.receive(kHost, encode(report), 0, consume) == Exchange::Admission::Accepted);
    CHECK(third.hostReceipt(2) == 1);
    // A direct link that goes leaves the figure in force: what it brought may still lead.
    third.detach(302);
    CHECK(third.hostReceipt(2) == 1);
}
