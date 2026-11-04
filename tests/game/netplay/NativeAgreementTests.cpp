#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestSupport.h"
#include "fixtures/PacketStar.h"
#include "fixtures/RecordingPacketTransport.h"
#include "game/netplay/NativeAgreement.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = NativeAgreement::Admission;
using Failure = NativeAgreement::Failure;
using Digest = NativeAgreement::Digest;
using Time = NativeAgreement::Time;
constexpr u64 kEpoch = 17;
constexpr PacketTransport::Connection kHost = 99;
constexpr test::StarLinks kStar{kHost};
constexpr u32 kPeriod = 60;

Digest digest(u64 tick) {
    Digest value{};
    for (usize index = 0; index < value.size(); ++index) {
        value[index] = static_cast<u8>(tick + 11 * index);
    }
    return value;
}
std::vector<u8> packet(u8 kind, u64 epoch, u64 tick, const Digest& value,
                       Failure reason = Failure::None) {
    std::vector<u8> bytes{
        'G', 'D', 'N', 'A', NativeAgreement::kVersion, kind, static_cast<u8>(reason), 0};
    for (const u64 wide : {epoch, tick}) {
        for (u32 shift = 0; shift < 64; shift += 8) {
            bytes.push_back(static_cast<u8>(wide >> shift));
        }
    }
    bytes.insert(bytes.end(), value.begin(), value.end());
    return bytes;
}

using Wire = test::RecordingPacketTransport;

struct Room : test::PacketStar<> {
    std::array<NativeAgreement, kSlots> streams;
    Time now;
    u32 period;
    explicit Room(NativeAgreement::Owners roster = {1, 2, 0, 0}, u32 cadence = kPeriod)
        : PacketStar(roster, kStar), period(cadence) {
        std::vector<NativeAgreement::Link> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, connection(peer)});
                const std::array host{NativeAgreement::Link{1, kHost}};
                REQUIRE(streams[peer].open(peer, kEpoch, owners, host, period, now));
            }
        }
        REQUIRE(streams[1].open(1, kEpoch, owners, links, period, now));
    }
    static PacketTransport::Connection connection(u8 peer) { return kStar.guest(peer); }
    /** What a delivered report or verdict meets: its checks, then its receiver. */
    auto arrival() {
        return [this](u8 /*source*/, u8 target, PacketTransport::Connection sender,
                      const Packet& message) {
            CHECK(message.delivery == PacketTransport::Delivery::Reliable);
            CHECK(message.bytes.size() == NativeAgreement::kPacketBytes);
            const auto result = streams[target].receive(sender, message.bytes, now);
            CHECK((result == Admission::Accepted || result == Admission::Duplicate ||
                   result == Admission::Failed));
        };
    }
    void deliver(u8 source) { PacketStar::deliver(source, arrival()); }
    void pump() {
        PacketStar::pump(
            3,
            [this](u8 peer, Wire& wire) {
                const bool flushed = streams[peer].flush(wire, now);
                CHECK(flushed == !streams[peer].failed());
            },
            arrival());
    }
    void submitAll(u64 tick, const Digest& value) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(streams[peer].submit(tick, value, now) == Admission::Accepted);
            }
        }
    }
};
} // namespace

TEST_CASE("native agreement validates the exact authenticated frozen star",
          "[netplay][native-agreement]") {
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    NativeAgreement stream;
    u8 local = 1;
    u64 epoch = kEpoch;
    NativeAgreement::Owners owners{1, 2, 0, 0};
    std::vector<NativeAgreement::Link> links{{2, 102}};
    u32 period = kPeriod;
    switch (mutation) {
    case 0: local = 0; break;
    case 1: local = 5; break;
    case 2: epoch = 0; break;
    case 3: owners = {2, 2, 0, 0}; break;
    case 4: owners[3] = 5; break;
    case 5: links.clear(); break;
    case 6: links[0].connection = 0; break;
    case 7: links.push_back(links.front()); break;
    case 8: links[0].peer = 3; break;
    case 9: period = 0; break;
    case 10: period = NativeAgreement::kMaxPeriod + 1; break;
    case 11:
        owners[2] = 3;
        links.push_back({3, 102});
        break;
    case 12:
        local = 2;
        links = {{1, kHost}, {3, 103}};
        owners[2] = 3;
        break;
    default: break;
    }
    CHECK_FALSE(stream.open(local, epoch, owners, links, period, Time{}));
    CHECK_FALSE(stream.canSubmit());
    CHECK(stream.pendingTicks() == 0);
    CHECK(stream.queuedPackets() == 0);
    const std::array validLinks{NativeAgreement::Link{2, 102}};
    REQUIRE(stream.open(1, kEpoch, {1, 2, 0, 0}, validLinks, kPeriod, Time{}));
    CHECK_FALSE(stream.open(1, kEpoch + 1, {1, 2, 0, 0}, validLinks, kPeriod, Time{}));
}

TEST_CASE("native agreement requires every peer including the host once per scheduled tick",
          "[netplay][native-agreement]") {
    const auto roster =
        GENERATE((NativeAgreement::Owners{1, 2, 0, 0}), (NativeAgreement::Owners{1, 2, 3, 0}),
                 (NativeAgreement::Owners{1, 2, 3, 4}), (NativeAgreement::Owners{2, 1, 2, 4}));
    Room room(roster);
    for (u64 sequence = 1; sequence <= 40; ++sequence) {
        const u64 tick = sequence * kPeriod;
        const auto value = sequence == 1 ? Digest{} : digest(tick);
        // All remote reports cannot agree without the host's own observation.
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (room.present(peer)) {
                REQUIRE(room.streams[peer].submit(tick, value, room.now) == Admission::Accepted);
            }
        }
        room.pump();
        CHECK(room.streams[1].agreedTick() == tick - kPeriod);
        REQUIRE(room.streams[1].submit(tick, value, room.now) == Admission::Accepted);
        room.pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room.present(peer)) {
                CHECK(room.streams[peer].agreedTick() == tick);
                CHECK(room.streams[peer].submitTick() == tick + kPeriod);
                CHECK(room.streams[peer].pendingTicks() == 0);
                CHECK(room.streams[peer].queuedPackets() == 0);
                CHECK(room.streams[peer].canSubmit());
            }
        }
    }
}

TEST_CASE("a suspended peer no longer holds the host's agreement and is told nothing",
          "[netplay][native-agreement][drop]") {
    Room room({1, 2, 3, 0});
    REQUIRE(room.streams[1].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
    REQUIRE(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
    REQUIRE(room.streams[2].flush(room.wires[2], room.now));
    room.deliver(2);
    CHECK(room.streams[1].agreedTick() == 0); // peer three has not reported
    room.streams[1].suspend(3, room.now);
    CHECK(room.streams[1].agreedTick() == kPeriod);
    CHECK_FALSE(room.streams[1].failed());
    REQUIRE(room.streams[1].flush(room.wires[1], room.now));
    for (const auto& message : room.wires[1].sent) {
        CHECK(message.connection == Room::connection(2));
    }
    CHECK_FALSE(room.wires[1].sent.empty());
    room.deliver(1);
    CHECK(room.streams[2].agreedTick() == kPeriod);
}

TEST_CASE("native agreement does not compare a partial peer roster",
          "[netplay][native-agreement]") {
    Room room({1, 2, 3, 4});
    REQUIRE(room.streams[1].submit(kPeriod, digest(1), room.now) == Admission::Accepted);
    REQUIRE(room.streams[2].submit(kPeriod, digest(2), room.now) == Admission::Accepted);
    REQUIRE(room.streams[3].submit(kPeriod, digest(1), room.now) == Admission::Accepted);
    room.pump();
    CHECK_FALSE(room.streams[1].failed());
    CHECK(room.streams[1].agreedTick() == 0);
    REQUIRE(room.streams[4].submit(kPeriod, digest(1), room.now) == Admission::Accepted);
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.streams[peer].failure() == Failure::Mismatch);
        CHECK(room.streams[peer].failureTick() == kPeriod);
        CHECK(room.streams[peer].agreedTick() == 0);
        CHECK_FALSE(room.streams[peer].canSubmit());
        CHECK(room.streams[peer].pendingTicks() == 0);
    }
}

TEST_CASE("native agreement compares all digest bytes and never repairs a mismatch",
          "[netplay][native-agreement]") {
    for (usize index = 0; index < Digest{}.size(); ++index) {
        CAPTURE(index);
        Room room;
        auto changed = digest(kPeriod);
        changed[index] ^= 0x80;
        REQUIRE(room.streams[1].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
        REQUIRE(room.streams[2].submit(kPeriod, changed, room.now) == Admission::Accepted);
        room.pump();
        CHECK(room.streams[1].failure() == Failure::Mismatch);
        CHECK(room.streams[2].failure() == Failure::Mismatch);
        CHECK(room.streams[1].submit(kPeriod, changed, room.now) == Admission::Failed);
        CHECK(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Failed);
    }
}

TEST_CASE("native agreement retains exact duplicates and rejects conflicting duplicates",
          "[netplay][native-agreement]") {
    const bool completed = GENERATE(false, true);
    const bool local = GENERATE(false, true);
    Room room;
    const auto report = packet(1, kEpoch, kPeriod, digest(kPeriod));
    REQUIRE(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
    CHECK(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Duplicate);
    CHECK(room.streams[2].queuedPackets() == 1);
    REQUIRE(room.streams[1].receive(102, report, room.now) == Admission::Accepted);
    CHECK(room.streams[1].receive(102, report, room.now) == Admission::Duplicate);
    if (completed) {
        REQUIRE(room.streams[1].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
        room.pump();
        CHECK(room.streams[1].agreedTick() == kPeriod);
    }
    if (local) {
        CHECK(room.streams[2].submit(kPeriod, digest(kPeriod + 1), room.now) == Admission::Failed);
    } else {
        CHECK(room.streams[1].receive(102, packet(1, kEpoch, kPeriod, digest(kPeriod + 1)),
                                      room.now) == Admission::Failed);
    }
    room.pump();
    CHECK(room.streams[1].failure() == Failure::Conflict);
    CHECK(room.streams[2].failure() == Failure::Conflict);
}

TEST_CASE("native agreement handles reordered reports and verdicts without skipping intervals",
          "[netplay][native-agreement]") {
    Room room;
    for (u64 const tick : {u64{kPeriod}, static_cast<u64>(2 * kPeriod)}) {
        room.submitAll(tick, digest(tick));
    }
    REQUIRE(room.streams[2].flush(room.wires[2], room.now));
    REQUIRE(room.wires[2].sent.size() == 2);
    const auto reports = room.wires[2].sent;
    room.wires[2].sent.clear();
    CHECK(room.streams[1].receive(102, reports[1].bytes, room.now) == Admission::Accepted);
    CHECK(room.streams[1].agreedTick() == 0);
    CHECK(room.streams[1].receive(102, reports[0].bytes, room.now) == Admission::Accepted);
    CHECK(room.streams[1].agreedTick() == static_cast<u64>(2) * kPeriod);
    REQUIRE(room.streams[1].flush(room.wires[1], room.now));
    REQUIRE(room.wires[1].sent.size() == 2);
    const auto verdicts = room.wires[1].sent;
    CHECK(room.streams[2].receive(kHost, verdicts[1].bytes, room.now) == Admission::Accepted);
    CHECK(room.streams[2].agreedTick() == 0);
    CHECK(room.streams[2].receive(kHost, verdicts[1].bytes, room.now) == Admission::Duplicate);
    CHECK(room.streams[2].receive(kHost, verdicts[0].bytes, room.now) == Admission::Accepted);
    CHECK(room.streams[2].agreedTick() == static_cast<u64>(2) * kPeriod);
    CHECK(room.streams[2].receive(kHost, verdicts[0].bytes, room.now) == Admission::Duplicate);
}

TEST_CASE("native agreement authenticates report and verdict roles and rejects stale epochs",
          "[netplay][native-agreement]") {
    Room room;
    room.submitAll(kPeriod, digest(kPeriod));
    const auto report = packet(1, kEpoch, kPeriod, digest(kPeriod));
    const auto accepted = packet(2, kEpoch, kPeriod, digest(kPeriod));
    CHECK(room.streams[1].receive(999, report, Time::max()) == Admission::WrongPeer);
    CHECK(room.streams[2].receive(999, accepted, Time::max()) == Admission::WrongPeer);
    CHECK(room.streams[1].receive(0, report, Time::max()) == Admission::WrongPeer);
    CHECK(room.streams[1].receive(102, packet(1, kEpoch - 1, kPeriod, digest(kPeriod)),
                                  Time::max()) == Admission::Stale);
    CHECK(room.streams[2].receive(kHost, packet(3, kEpoch + 1, 0, {}, Failure::Timeout),
                                  Time::max()) == Admission::Stale);
    CHECK_FALSE(room.streams[1].failed());
    CHECK_FALSE(room.streams[2].failed());
    room.pump();
    CHECK(room.streams[2].agreedTick() == kPeriod);
    SECTION("a guest cannot issue host acceptance") {
        CHECK(room.streams[1].receive(102, accepted, room.now) == Admission::Invalid);
        CHECK(room.streams[1].failure() == Failure::Protocol);
    }
    SECTION("a host cannot relay reports as a guest verdict") {
        CHECK(room.streams[2].receive(kHost, report, room.now) == Admission::Invalid);
        CHECK(room.streams[2].failure() == Failure::Protocol);
    }
}

TEST_CASE("terminal agreement has one exact u64 tick and a separate periodic wire domain",
          "[netplay][native-agreement][native-terminal-agreement]") {
    const u64 tick = GENERATE(u64{31}, u64{60}, u64{3601}, u64{1} << 40);
    const bool mismatch = GENERATE(false, true);
    NativeAgreement host;
    NativeAgreement guest;
    NativeAgreement periodic;
    const NativeAgreement::Owners owners{1, 2, 0, 0};
    const std::array hostLinks{NativeAgreement::Link{2, 102}};
    const std::array guestLinks{NativeAgreement::Link{1, kHost}};
    REQUIRE(host.openTerminal(1, kEpoch, owners, hostLinks, tick, Time{}));
    REQUIRE(guest.openTerminal(2, kEpoch, owners, guestLinks, tick, Time{}));
    REQUIRE(periodic.open(1, kEpoch, owners, hostLinks, kPeriod, Time{}));
    CHECK(host.canSubmit());
    CHECK(host.submit(tick + 1, digest(tick), Time{}) == Admission::Invalid);
    REQUIRE(host.submit(tick, digest(tick), Time{}) == Admission::Accepted);
    CHECK_FALSE(host.canSubmit());
    auto remote = digest(tick);
    if (mismatch) {
        remote.back() ^= 1;
    }
    REQUIRE(guest.submit(tick, remote, Time{}) == Admission::Accepted);
    Wire report;
    REQUIRE(guest.flush(report, Time{}));
    REQUIRE(report.sent.size() == 1);
    const auto& bytes = report.sent.front().bytes;
    CHECK(NativeAgreement::recognizesTerminal(bytes));
    CHECK(NativeAgreement::terminalContext(bytes) == std::pair{kEpoch, tick});
    CHECK(periodic.receive(102, bytes, Time::max()) == Admission::Stale);
    CHECK_FALSE(periodic.failed());
    CHECK(host.receive(102, packet(1, kEpoch, tick, digest(tick)), Time::max()) ==
          Admission::Stale);
    CHECK_FALSE(host.failed());
    CHECK(host.receive(102, bytes, Time{}) == (mismatch ? Admission::Failed : Admission::Accepted));
    Wire verdict;
    CHECK(host.flush(verdict, Time{}) == !mismatch);
    REQUIRE(verdict.sent.size() == 1);
    CHECK(NativeAgreement::recognizesTerminal(verdict.sent.front().bytes));
    CHECK(guest.receive(kHost, verdict.sent.front().bytes, Time{}) ==
          (mismatch ? Admission::Failed : Admission::Accepted));
    if (mismatch) {
        CHECK(host.failure() == Failure::Mismatch);
        CHECK(guest.failure() == Failure::Mismatch);
    } else {
        CHECK(host.agreedTick() == tick);
        CHECK(guest.agreedTick() == tick);
        CHECK(host.submit(tick, digest(tick), Time{}) == Admission::Duplicate);
        CHECK_FALSE(host.canSubmit());
    }
}

TEST_CASE("native agreement wire is exact bounded canonical little endian",
          "[netplay][native-agreement]") {
    Room room;
    REQUIRE(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
    REQUIRE(room.streams[2].flush(room.wires[2], room.now));
    REQUIRE(room.wires[2].sent.size() == 1);
    const auto& actual = room.wires[2].sent.front().bytes;
    CHECK(actual == packet(1, kEpoch, kPeriod, digest(kPeriod)));
    CHECK(NativeAgreement::recognizes(actual));
    CHECK(NativeAgreement::recognizes(std::span{actual}.first(4)));
    CHECK_FALSE(NativeAgreement::recognizes(std::span{actual}.first(3)));
    for (usize size = 0; size < actual.size(); ++size) {
        Room truncated;
        CHECK(truncated.streams[1].receive(102, std::span{actual}.first(size), Time{}) ==
              Admission::Invalid);
        CHECK(truncated.streams[1].failure() == Failure::Protocol);
    }
    auto oversized = actual;
    oversized.push_back(0);
    CHECK(room.streams[1].receive(102, oversized, room.now) == Admission::Invalid);
    CHECK(room.streams[1].failure() == Failure::Protocol);
}

TEST_CASE("native agreement packets keep their byte layout", "[netplay][native-agreement]") {
    constexpr u64 kWideEpoch = 0x1122334455667788ULL;
    constexpr u64 kTerminalTick = 0x0123456789abcdefULL;
    const NativeAgreement::Owners owners{1, 2, 0, 0};
    const std::array hostLinks{NativeAgreement::Link{2, 102}};
    const std::array guestLinks{NativeAgreement::Link{1, kHost}};
    NativeAgreement host;
    NativeAgreement guest;
    REQUIRE(host.open(1, kWideEpoch, owners, hostLinks, kPeriod, Time{}));
    REQUIRE(guest.open(2, kWideEpoch, owners, guestLinks, kPeriod, Time{}));
    REQUIRE(guest.submit(kPeriod, digest(kPeriod), Time{}) == Admission::Accepted);
    Wire report;
    REQUIRE(guest.flush(report, Time{}));
    REQUIRE(report.sent.size() == 1);
    constexpr std::string_view kReport =
        "47444e41"                          // magic
        "01010000"                          // version, report, no failure, zero
        "8877665544332211"                  // epoch
        "3c00000000000000"                  // tick
        "3c47525d68737e89949faab5c0cbd6e1"  // digest, bytes 0 to 15
        "ecf7020d18232e39444f5a65707b8691"; // digest, bytes 16 to 31
    CHECK(test::hexBytes(report.sent.front().bytes) == kReport);

    REQUIRE(host.submit(kPeriod, digest(kPeriod), Time{}) == Admission::Accepted);
    REQUIRE(host.receive(102, report.sent.front().bytes, Time{}) == Admission::Accepted);
    Wire verdict;
    REQUIRE(host.flush(verdict, Time{}));
    REQUIRE(verdict.sent.size() == 1);
    constexpr std::string_view kVerdict =
        "47444e41"                          // magic
        "01020000"                          // version, accepted, no failure, zero
        "8877665544332211"                  // epoch
        "3c00000000000000"                  // tick
        "3c47525d68737e89949faab5c0cbd6e1"  // digest, bytes 0 to 15
        "ecf7020d18232e39444f5a65707b8691"; // digest, bytes 16 to 31
    CHECK(test::hexBytes(verdict.sent.front().bytes) == kVerdict);

    NativeAgreement terminal;
    REQUIRE(terminal.openTerminal(2, kWideEpoch, owners, guestLinks, kTerminalTick, Time{}));
    REQUIRE(terminal.submit(kTerminalTick, digest(7), Time{}) == Admission::Accepted);
    Wire terminalReport;
    REQUIRE(terminal.flush(terminalReport, Time{}));
    REQUIRE(terminalReport.sent.size() == 1);
    constexpr std::string_view kTerminal =
        "47444e54"                          // terminal magic
        "01010000"                          // version, report, no failure, zero
        "8877665544332211"                  // epoch
        "efcdab8967452301"                  // tick
        "07121d28333e49545f6a75808b96a1ac"  // digest, bytes 0 to 15
        "b7c2cdd8e3eef9040f1a25303b46515c"; // digest, bytes 16 to 31
    CHECK(test::hexBytes(terminalReport.sent.front().bytes) == kTerminal);

    NativeAgreement failing;
    REQUIRE(failing.open(2, kWideEpoch, owners, guestLinks, kPeriod, Time{}));
    const std::array<u8, 3> garbage{1, 2, 3};
    REQUIRE(failing.receive(kHost, garbage, Time{}) == Admission::Invalid);
    Wire notice;
    CHECK_FALSE(failing.flush(notice, Time{}));
    REQUIRE(notice.sent.size() == 1);
    constexpr std::string_view kFailure =
        "47444e41"                          // magic
        "01030500"                          // version, failed, protocol, zero
        "8877665544332211"                  // epoch
        "0000000000000000"                  // tick
        "00000000000000000000000000000000"  // digest, bytes 0 to 15
        "00000000000000000000000000000000"; // digest, bytes 16 to 31
    CHECK(test::hexBytes(notice.sent.front().bytes) == kFailure);
}

TEST_CASE("native agreement refuses malformed flags and noncanonical terminal messages",
          "[netplay][native-agreement]") {
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    Room room;
    auto bytes = packet(1, kEpoch, kPeriod, digest(kPeriod));
    switch (mutation) {
    case 0: bytes[0] ^= 1; break;
    case 1: bytes[4] = NativeAgreement::kVersion + 1; break;
    case 2: bytes[5] = 0; break;
    case 3: bytes[5] = 4; break;
    case 4: bytes[6] = 255; break;
    case 5: bytes[7] = 1; break;
    case 6: bytes = packet(1, 0, kPeriod, digest(kPeriod)); break;
    case 7: bytes = packet(1, kEpoch, 0, digest(kPeriod)); break;
    case 8: bytes = packet(1, kEpoch, kPeriod + 1, digest(kPeriod)); break;
    case 9: bytes = packet(3, kEpoch, kPeriod, {}, Failure::None); break;
    case 10: bytes = packet(3, kEpoch, kPeriod, digest(kPeriod), Failure::Mismatch); break;
    case 11: bytes = packet(1, kEpoch, kPeriod, digest(kPeriod), Failure::Mismatch); break;
    default: break;
    }
    CHECK(room.streams[1].receive(102, bytes, room.now) == Admission::Invalid);
    CHECK(room.streams[1].failure() == Failure::Protocol);
    CHECK(room.streams[1].agreedTick() == 0);
    CHECK(room.streams[1].pendingTicks() == 0);
    CHECK(room.streams[1].queuedPackets() == 1);
}

TEST_CASE("native agreement submission cannot omit ticks or claim an unsubmitted verdict",
          "[netplay][native-agreement]") {
    Room room;
    CHECK(room.streams[2].submit(0, {}, room.now) == Admission::Invalid);
    CHECK(room.streams[2].submit(kPeriod + 1, {}, room.now) == Admission::Invalid);
    CHECK(room.streams[2].submit(static_cast<u64>(2) * kPeriod, {}, room.now) ==
          Admission::Invalid);
    CHECK(room.streams[2].submitTick() == kPeriod);
    CHECK(room.streams[2].queuedPackets() == 0);
    CHECK(room.streams[2].receive(kHost, packet(2, kEpoch, kPeriod, {}), room.now) ==
          Admission::Invalid);
    CHECK(room.streams[2].failure() == Failure::Protocol);
}

TEST_CASE("native agreement guests verify the exact host-accepted digest",
          "[netplay][native-agreement]") {
    Room room({1, 2, 3, 4});
    REQUIRE(room.streams[2].submit(kPeriod, digest(kPeriod), room.now) == Admission::Accepted);
    CHECK(room.streams[2].receive(kHost, packet(2, kEpoch, kPeriod, digest(kPeriod + 1)),
                                  room.now) == Admission::Failed);
    CHECK(room.streams[2].failure() == Failure::Mismatch);
    CHECK(room.streams[2].agreedTick() == 0);
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.streams[peer].failure() == Failure::Mismatch);
        CHECK(room.streams[peer].failureTick() == kPeriod);
    }
}

TEST_CASE("native agreement bounds pending reports and preserves congested delivery exactly",
          "[netplay][native-agreement]") {
    Room room;
    room.wires[2].result = PacketTransport::SendResult::Congested;
    for (u64 index = 1; index <= NativeAgreement::kWindow; ++index) {
        REQUIRE(room.streams[2].submit(index * kPeriod, digest(index), room.now) ==
                Admission::Accepted);
    }
    CHECK_FALSE(room.streams[2].canSubmit());
    CHECK(room.streams[2].pendingTicks() == NativeAgreement::kWindow);
    CHECK(room.streams[2].queuedPackets() == NativeAgreement::kQueuedPackets);
    for (u32 attempt = 0; attempt < 100; ++attempt) {
        CHECK(room.streams[2].submit((NativeAgreement::kWindow + 1) * kPeriod, {}, room.now) ==
              Admission::Full);
        REQUIRE(room.streams[2].flush(room.wires[2], room.now));
        CHECK(room.streams[2].queuedPackets() == NativeAgreement::kQueuedPackets);
    }
    CHECK(room.wires[2].sent.empty());
    room.wires[2].result = PacketTransport::SendResult::Sent;
    room.wires[2].budget = 3;
    REQUIRE(room.streams[2].flush(room.wires[2], room.now));
    CHECK(room.streams[2].queuedPackets() == NativeAgreement::kQueuedPackets - 3);
    for (usize index = 0; index < room.wires[2].sent.size(); ++index) {
        CHECK(room.wires[2].sent[index].bytes ==
              packet(1, kEpoch, (index + 1) * kPeriod, digest(index + 1)));
    }
    room.deliver(2);
    room.wires[2].budget = std::numeric_limits<usize>::max();
    for (u64 index = 1; index <= NativeAgreement::kWindow; ++index) {
        REQUIRE(room.streams[1].submit(index * kPeriod, digest(index), room.now) ==
                Admission::Accepted);
    }
    room.pump();
    CHECK(room.streams[2].agreedTick() == NativeAgreement::kWindow * kPeriod);
    CHECK(room.streams[2].canSubmit());
    CHECK(room.streams[2].submit((NativeAgreement::kWindow + 1) * kPeriod, {}, room.now) ==
          Admission::Accepted);
}

TEST_CASE("native agreement bounds host verdict queues and fails overflowing remote windows",
          "[netplay][native-agreement]") {
    SECTION("host verdict congestion stalls local submission without losing verdicts") {
        Room room;
        room.wires[1].result = PacketTransport::SendResult::Congested;
        for (u64 index = 1; index <= NativeAgreement::kQueuedPackets; ++index) {
            const auto tick = index * kPeriod;
            room.submitAll(tick, digest(tick));
            room.pump();
        }
        CHECK(room.streams[1].queuedPackets() == NativeAgreement::kQueuedPackets);
        CHECK(room.streams[1].agreedTick() == NativeAgreement::kQueuedPackets * kPeriod);
        CHECK(room.streams[1].pendingTicks() == 0);
        CHECK_FALSE(room.streams[1].canSubmit());
        CHECK(room.streams[1].submit((NativeAgreement::kQueuedPackets + 1) * kPeriod, {},
                                     room.now) == Admission::Full);
        room.wires[1].result = PacketTransport::SendResult::Sent;
        room.pump();
        CHECK(room.streams[1].canSubmit());
        CHECK(room.streams[2].agreedTick() == NativeAgreement::kQueuedPackets * kPeriod);
    }
    SECTION("remote overflow is a terminal refusal, never silent report loss") {
        Room room({1, 2, 3, 4}, 1);
        CHECK(room.streams[1].receive(102, packet(1, kEpoch, std::numeric_limits<u64>::max(), {}),
                                      room.now) == Admission::Full);
        CHECK(room.streams[1].failure() == Failure::Window);
        CHECK(room.streams[1].pendingTicks() == 0);
        CHECK(room.streams[1].queuedPackets() == 3);
    }
}

TEST_CASE("native agreement real-time deadlines do not renew on traffic or congestion",
          "[netplay][native-agreement]") {
    const auto mode = GENERATE(0, 1, 2);
    Room room;
    if (mode == 0) {
        REQUIRE(room.streams[1].submit(kPeriod, {}, room.now) == Admission::Accepted);
    } else if (mode == 1) {
        REQUIRE(room.streams[2].submit(kPeriod, {}, room.now) == Admission::Accepted);
        REQUIRE(room.streams[2].flush(room.wires[2], room.now)); // awaiting host verdict
    } else {
        room.submitAll(kPeriod, {});
        REQUIRE(room.streams[2].flush(room.wires[2], room.now));
        room.deliver(2); // host has agreed; its outgoing verdict still has a deadline
        CHECK(room.streams[1].pendingTicks() == 0);
    }
    auto& stream = room.streams[mode == 1 ? 2 : 1];
    // A guest waits on the host longer than the host waits on its guests.
    const auto limit = mode == 1 ? NativeAgreement::kGuestTimeout : NativeAgreement::kTimeout;
    CHECK(stream.update(Time{} + std::chrono::seconds{9}));
    CHECK(stream.update(Time::min()));
    CHECK(stream.submit(kPeriod, {}, Time{} + std::chrono::seconds{9}) == Admission::Duplicate);
    CHECK(stream.update(Time{} + limit - std::chrono::seconds{1}));
    CHECK(stream.overdue(Time{} + limit - std::chrono::seconds{1}).empty());
    if (mode != 1) {
        // The host goes on and names the guest it waits on, for the session to drop.
        CHECK(stream.update(Time{} + limit));
        CHECK(stream.overdue(Time{} + limit) == std::vector<u8>{2});
        return;
    }
    CHECK_FALSE(stream.update(Time{} + limit));
    CHECK(stream.failure() == Failure::Timeout);
    CHECK(stream.failureTick() == kPeriod);
    room.now = Time{} + limit;
    room.pump();
    CHECK(room.streams[1].failed());
    CHECK(room.streams[2].failed());
}

TEST_CASE("the host names the guests an overdue tick still waits on, never itself",
          "[netplay][native-agreement][drop]") {
    Room room({1, 2, 3, 0});
    const auto late = Time{} + NativeAgreement::kTimeout;
    SECTION("a guest's report is missing") {
        REQUIRE(room.streams[1].submit(kPeriod, {}, room.now) == Admission::Accepted);
        REQUIRE(room.streams[3].submit(kPeriod, {}, room.now) == Admission::Accepted);
        room.pump();
        CHECK(room.streams[1].overdue(late - std::chrono::seconds{1}).empty());
        CHECK(room.streams[1].overdue(late) == std::vector<u8>{2});
        CHECK(room.streams[2].overdue(late).empty()); // only the host judges
        room.streams[1].suspend(2, late);
        CHECK(room.streams[1].agreedTick() == kPeriod);
        CHECK(room.streams[1].overdue(late).empty()); // the verdict to peer three goes out now
    }
    SECTION("the host's own digest is the late one") {
        REQUIRE(room.streams[2].submit(kPeriod, {}, room.now) == Admission::Accepted);
        room.pump();
        CHECK(room.streams[1].overdue(late).empty());
        CHECK_FALSE(room.streams[1].update(late));
    }
}

TEST_CASE("a repair starts every agreement wait over", "[netplay][native-agreement]") {
    Room room;
    REQUIRE(room.streams[1].submit(kPeriod, {}, room.now) == Admission::Accepted);
    REQUIRE(room.streams[2].submit(kPeriod, {}, room.now) == Admission::Accepted);
    const auto repaired = Time{} + std::chrono::seconds{8};
    room.streams[1].rearm(repaired);
    room.streams[2].rearm(repaired);
    // The host's wait on its guest's report, and the guest's on its verdict, run from then.
    CHECK(room.streams[1].overdue(Time{} + NativeAgreement::kTimeout).empty());
    CHECK(room.streams[1].overdue(repaired + NativeAgreement::kTimeout) == std::vector<u8>{2});
    CHECK(room.streams[2].update(Time{} + NativeAgreement::kGuestTimeout));
    CHECK_FALSE(room.streams[2].update(repaired + NativeAgreement::kGuestTimeout));
}

TEST_CASE("native agreement idle time and extreme injected clocks cannot invent expiry",
          "[netplay][native-agreement]") {
    SECTION("no reports and no queued delivery means no timeout") {
        Room room;
        CHECK(room.streams[1].update(Time::max()));
        CHECK(room.streams[2].update(Time::max()));
        CHECK_FALSE(room.streams[1].failed());
        CHECK_FALSE(room.streams[2].failed());
    }
    SECTION("pending reports expire across the complete signed clock range") {
        NativeAgreement stream;
        const std::array links{NativeAgreement::Link{2, 102}};
        REQUIRE(stream.open(1, kEpoch, {1, 2, 0, 0}, links, kPeriod, Time::min()));
        REQUIRE(stream.submit(kPeriod, {}, Time::min()) == Admission::Accepted);
        CHECK(stream.update(Time::max())); // the host goes on and names the guest it waits on
        CHECK(stream.overdue(Time::max()) == std::vector<u8>{2});
        NativeAgreement guest;
        const std::array host{NativeAgreement::Link{1, 100}};
        REQUIRE(guest.open(2, kEpoch, {1, 2, 0, 0}, host, kPeriod, Time::min()));
        REQUIRE(guest.submit(kPeriod, {}, Time::min()) == Admission::Accepted);
        CHECK_FALSE(guest.update(Time::max()));
        CHECK(guest.failure() == Failure::Timeout);
    }
    SECTION("a start near max cannot overflow its timeout into the past") {
        NativeAgreement stream;
        const std::array links{NativeAgreement::Link{2, 102}};
        const auto start = Time::max() - std::chrono::seconds{1};
        REQUIRE(stream.open(1, kEpoch, {1, 2, 0, 0}, links, kPeriod, start));
        REQUIRE(stream.submit(kPeriod, {}, start) == Admission::Accepted);
        CHECK(stream.update(Time::max()));
        CHECK(stream.overdue(Time::max()).empty());
        NativeAgreement guest;
        const std::array host{NativeAgreement::Link{1, 100}};
        REQUIRE(guest.open(2, kEpoch, {1, 2, 0, 0}, host, kPeriod, start));
        REQUIRE(guest.submit(kPeriod, {}, start) == Admission::Accepted);
        CHECK(guest.update(Time::max()));
    }
}

TEST_CASE("native agreement terminal failures have reserved bounded reliable capacity",
          "[netplay][native-agreement]") {
    Room room({1, 2, 3, 4});
    room.wires[1].result = PacketTransport::SendResult::Congested;
    for (u64 index = 1; index <= NativeAgreement::kQueuedPackets; ++index) {
        room.submitAll(index * kPeriod, digest(index));
        room.pump();
    }
    REQUIRE(room.streams[1].queuedPackets() == 3 * NativeAgreement::kQueuedPackets);
    CHECK(room.streams[1].receive(102, packet(1, kEpoch, kPeriod, digest(999)), room.now) ==
          Admission::Failed);
    CHECK(room.streams[1].failure() == Failure::Conflict);
    for (u32 retry = 0; retry < 100; ++retry) {
        CHECK_FALSE(room.streams[1].flush(room.wires[1], room.now));
        CHECK(room.streams[1].queuedPackets() == 3);
    }
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.streams[peer].failure() == Failure::Conflict);
        CHECK(room.streams[peer].failureTick() == kPeriod);
        CHECK(room.streams[peer].queuedPackets() == 0);
    }
}

TEST_CASE("native agreement disconnect and fatal send notify surviving peers without neutral "
          "substitution",
          "[netplay][native-agreement]") {
    const auto mode = GENERATE(0, 1, 2);
    Room room({1, 2, 3, 4});
    room.streams[1].disconnected(999);
    room.streams[1].disconnected(0);
    CHECK_FALSE(room.streams[1].failed());
    if (mode == 0) {
        room.streams[1].disconnected(102);
    } else {
        room.submitAll(kPeriod, {});
        for (u8 peer = 2; peer <= 4; ++peer) {
            REQUIRE(room.streams[peer].flush(room.wires[peer], room.now));
            room.deliver(peer);
        }
        room.wires[1].faultConnection = 102;
        room.wires[1].result = mode == 1 ? PacketTransport::SendResult::Disconnected
                                         : PacketTransport::SendResult::Invalid;
        CHECK_FALSE(room.streams[1].flush(room.wires[1], room.now));
    }
    room.pump();
    const auto reason = mode == 0 ? Failure::Disconnected : Failure::Transport;
    CHECK(room.streams[1].failure() == reason);
    CHECK(room.streams[3].failure() == reason);
    CHECK(room.streams[4].failure() == reason);
    CHECK_FALSE(room.streams[2].failed()); // its independent transport must report disconnection
    room.streams[2].disconnected(kHost);
    CHECK(room.streams[2].failure() == Failure::Disconnected);
}

TEST_CASE("native agreement reports a closed send before its owner drops the departed peer",
          "[netplay][native-agreement][native-terminal-agreement]") {
    const bool terminal = GENERATE(false, true);
    const NativeAgreement::Owners owners{1, 2, 3, 0};
    const std::array hostLinks{NativeAgreement::Link{2, 102}, NativeAgreement::Link{3, 103}};
    const std::array guestLinks{NativeAgreement::Link{1, kHost}};
    std::array<NativeAgreement, 4> streams;
    std::array<Wire, 4> wires;
    for (u8 peer = 1; peer <= 3; ++peer) {
        const std::span<const NativeAgreement::Link> links =
            peer == 1 ? std::span<const NativeAgreement::Link>{hostLinks}
                      : std::span<const NativeAgreement::Link>{guestLinks};
        REQUIRE((terminal ? streams[peer].openTerminal(peer, kEpoch, owners, links, kPeriod, Time{})
                          : streams[peer].open(peer, kEpoch, owners, links, kPeriod, Time{})));
        REQUIRE(streams[peer].submit(kPeriod, digest(kPeriod), Time{}) == Admission::Accepted);
    }
    for (u8 peer = 2; peer <= 3; ++peer) {
        REQUIRE(streams[peer].flush(wires[peer], Time{}));
        REQUIRE(wires[peer].sent.size() == 1);
        REQUIRE(streams[1].receive(Room::connection(peer), wires[peer].sent.front().bytes,
                                   Time{}) == Admission::Accepted);
    }
    REQUIRE(streams[1].queuedPackets() == 2);
    wires[1].faultConnection = 102;
    wires[1].result = PacketTransport::SendResult::Disconnected;
    PacketTransport::Connection closed = 777;
    CHECK_FALSE(streams[1].flush(wires[1], Time{}, &closed));
    CHECK(closed == 102);
    CHECK_FALSE(streams[1].failed());
    CHECK(streams[1].queuedPackets() == 2);
    CHECK(wires[1].attempts == 1);

    // The session resolves the authenticated connection only after flush returns.
    streams[1].suspend(2, Time{});
    streams[3].suspend(2, Time{});
    REQUIRE(streams[1].flush(wires[1], Time{}, &closed));
    CHECK(closed == 0);
    REQUIRE(wires[1].sent.size() == 1);
    CHECK(wires[1].sent.front().connection == 103);
    CHECK(wires[1].attempts == 2);
    REQUIRE(streams[3].receive(kHost, wires[1].sent.front().bytes, Time{}) == Admission::Accepted);
    CHECK(streams[3].agreedTick() == kPeriod);
    CHECK_FALSE(streams[3].failed());
    CHECK(streams[1].queuedPackets() == 0);
}

TEST_CASE("native agreement's closed-link report does not make losing the host recoverable",
          "[netplay][native-agreement]") {
    Room room;
    auto& guest = room.streams[2];
    REQUIRE(guest.submit(kPeriod, {}, room.now) == Admission::Accepted);
    room.wires[2].result = PacketTransport::SendResult::Disconnected;
    PacketTransport::Connection closed = 0;
    CHECK_FALSE(guest.flush(room.wires[2], room.now, &closed));
    REQUIRE(closed == kHost);
    CHECK_FALSE(guest.failed());
    guest.disconnected(closed);
    CHECK(guest.failure() == Failure::Disconnected);
    CHECK_FALSE(guest.flush(room.wires[2], room.now, &closed));
    CHECK(closed == 0);
    CHECK(guest.failure() == Failure::Disconnected);
}

TEST_CASE("native agreement reports no closed link for congestion or generic send failure",
          "[netplay][native-agreement]") {
    const auto result =
        GENERATE(PacketTransport::SendResult::Sent, PacketTransport::SendResult::Congested,
                 PacketTransport::SendResult::Invalid);
    Room room;
    auto& guest = room.streams[2];
    REQUIRE(guest.submit(kPeriod, {}, room.now) == Admission::Accepted);
    room.wires[2].result = result;
    PacketTransport::Connection closed = 777;
    CHECK(guest.flush(room.wires[2], room.now, &closed) ==
          (result != PacketTransport::SendResult::Invalid));
    CHECK(closed == 0);
    CHECK(guest.failure() ==
          (result == PacketTransport::SendResult::Invalid ? Failure::Transport : Failure::None));
    CHECK(guest.queuedPackets() == (result == PacketTransport::SendResult::Congested ? 1 : 0));
}

TEST_CASE("native agreement retains a bounded duplicate history and does not revive old epochs",
          "[netplay][native-agreement]") {
    Room room;
    for (u64 sequence = 1; sequence <= NativeAgreement::kHistory + 2; ++sequence) {
        room.submitAll(sequence * kPeriod, digest(sequence));
        room.pump();
    }
    CHECK(room.streams[1].receive(102, packet(1, kEpoch, kPeriod, digest(999)), room.now) ==
          Admission::Stale);
    CHECK(room.streams[2].submit(kPeriod, digest(999), room.now) == Admission::Stale);
    CHECK_FALSE(room.streams[1].failed());
    const auto latest = (NativeAgreement::kHistory + 2) * kPeriod;
    CHECK(room.streams[1].receive(102, packet(1, kEpoch, latest, digest(999)), room.now) ==
          Admission::Failed);
    CHECK(room.streams[1].failure() == Failure::Conflict);
}

TEST_CASE("terminal epoch-wide failures retain routing context without a certificate tick",
          "[netplay][native-agreement][native-terminal-agreement]") {
    NativeAgreement failed;
    const std::array guestLinks{NativeAgreement::Link{1, kHost}};
    REQUIRE(failed.openTerminal(2, kEpoch, {1, 2, 0, 0}, guestLinks, 31, Time{}));
    const std::array<u8, 1> malformed{};
    REQUIRE(failed.receive(kHost, malformed, Time{}) == Admission::Invalid);
    REQUIRE(failed.failure() == Failure::Protocol);
    REQUIRE(failed.failureTick() == 0);
    Wire wire;
    CHECK_FALSE(failed.flush(wire, Time{}));
    REQUIRE(wire.sent.size() == 1);
    const auto& bytes = wire.sent.front().bytes;
    REQUIRE(NativeAgreement::recognizesTerminal(bytes));
    const auto context = NativeAgreement::terminalContext(bytes);
    REQUIRE(context);
    CHECK(context->first == kEpoch);
    CHECK(context->second == 0);

    const std::array hostLinks{NativeAgreement::Link{2, 102}};
    NativeAgreement current;
    REQUIRE(current.openTerminal(1, kEpoch, {1, 2, 0, 0}, hostLinks, 31, Time{}));
    CHECK(current.receive(102, bytes, Time{}) == Admission::Failed);
    CHECK(current.failure() == Failure::Protocol);
    CHECK(current.failureTick() == 0);
    NativeAgreement next;
    REQUIRE(next.openTerminal(1, kEpoch + 1, {1, 2, 0, 0}, hostLinks, 31, Time{}));
    CHECK(next.receive(102, bytes, Time{}) == Admission::Stale);
    CHECK_FALSE(next.failed());

    auto invalid = bytes;
    invalid[5] = 1; // A report, unlike a failure, must name a nonzero tick.
    invalid[6] = 0;
    CHECK_FALSE(NativeAgreement::terminalContext(invalid));
}
