#include <algorithm>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "fixtures/RecordingPacketTransport.h"
#include "game/netplay/PartyBootstrap.h"

namespace {
using namespace gdl;
using namespace gdl::game;
/** A recording wire that accepts a hundred sends and checks each goes reliably in one packet. */
struct Wire final : test::RecordingPacketTransport {
    Wire() { budget = 100; }

private:
    void inspect(Connection /*connection*/, std::span<const u8> bytes, Delivery delivery) override {
        CHECK(delivery == Delivery::Reliable);
        CHECK(bytes.size() <= kMaxPacketBytes);
    }
};
std::vector<CharacterProfile> selections(const MatchOwners& owners, u8 peer) {
    std::vector<CharacterProfile> result;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] == peer) {
            CharacterProfile profile;
            profile.name = "SEAT" + std::to_string(seat);
            profile.character = static_cast<s32>(seat);
            profile.color = static_cast<s32>(seat);
            profile.progress.inventory.keys = static_cast<s32>(seat);
            result.push_back(profile);
        }
    }
    return result;
}
TEST_CASE("party bootstrap relays every owned character and waits for every acknowledgement",
          "[netplay][party-bootstrap]") {
    MatchOwners owners{1, 2, 3, 4};
    SECTION("four machines") {}
    SECTION("two local plus two remote") {
        owners = {1, 2, 1, 2};
    }
    SECTION("host has sparse seats") {
        owners = {2, 0, 2, 1};
    }
    SECTION("one machine four seats") {
        owners = {1, 1, 1, 1};
    }
    std::array<PartyBootstrap, 5> peers;
    std::array<Wire, 5> wires;
    std::vector<MatchLink> links;
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (std::ranges::find(owners, peer) != owners.end()) {
            links.push_back({peer, peer});
            REQUIRE(peers[peer].open(peer, owners, std::array{MatchLink{1, 99}},
                                     selections(owners, peer)));
        }
    }
    REQUIRE(peers[1].open(1, owners, links, selections(owners, 1)));
    if (!links.empty()) {
        CHECK_FALSE(peers[1].complete());
    }
    for (const auto& link : links) {
        auto& wire = wires[link.peer];
        REQUIRE(peers[link.peer].flush(wire));
        for (const auto& message : wire.sent) {
            REQUIRE(peers[1].receive(link.connection, message.bytes));
        }
        wire.sent.clear();
        CHECK_FALSE(peers[1].complete());
    }
    // Only one reliable message is accepted per pump. No dropped tail or
    // unbounded re-enqueueing while the transport's send budget is exhausted.
    for (usize pump = 0; pump < 20; ++pump) {
        wires[1].budget = 1;
        REQUIRE(peers[1].flush(wires[1]));
        for (const auto& message : std::exchange(wires[1].sent, {})) {
            REQUIRE(peers[message.connection].receive(99, message.bytes));
        }
    }
    for (const auto& link : links) {
        CHECK_FALSE(peers[1].complete());
        REQUIRE(peers[link.peer].complete());
        REQUIRE(peers[link.peer].flush(wires[link.peer]));
        for (const auto& message : wires[link.peer].sent) {
            REQUIRE(peers[1].receive(link.connection, message.bytes));
        }
        for (usize seat = 0; seat < owners.size(); ++seat) {
            REQUIRE(peers[link.peer].party()[seat].has_value() == (owners[seat] != 0));
            if (owners[seat] != 0) {
                CHECK(CharacterProfilePacket::encode(*peers[link.peer].party()[seat]) ==
                      CharacterProfilePacket::encode(*peers[1].party()[seat]));
            }
        }
    }
    CHECK(peers[1].complete());
    CHECK_FALSE(peers[1].open(1, owners, links, selections(owners, 1)));
}

TEST_CASE("party bootstrap packets keep their byte layout", "[netplay][party-bootstrap]") {
    const MatchOwners owners{1, 2, 0, 0};
    const auto guestParty = selections(owners, 2);
    PartyBootstrap host;
    PartyBootstrap guest;
    Wire hostWire;
    Wire guestWire;
    REQUIRE(guest.open(2, owners, std::array{MatchLink{1, 99}}, guestParty));
    REQUIRE(host.open(1, owners, std::array{MatchLink{2, 2}}, selections(owners, 1)));
    REQUIRE(guest.flush(guestWire));
    REQUIRE(guestWire.sent.size() == 1);
    const auto profile = CharacterProfilePacket::encode(guestParty[0]);
    REQUIRE(profile);
    std::vector<u8> expected{'G', 'D', 'P', 'S', 1, 0, 1, 0};
    expected.insert(expected.end(), profile->begin(), profile->end());
    CHECK(guestWire.sent[0].bytes == expected);
    REQUIRE(host.receive(2, guestWire.sent[0].bytes));
    guestWire.sent.clear();
    REQUIRE(host.flush(hostWire));
    REQUIRE(hostWire.sent.size() == 3); // both profiles, then the seal
    CHECK(hostWire.sent[1].bytes == expected);
    constexpr std::string_view kSeal = "47445053"  // magic
                                       "01010000"; // version, seal, seat, zero
    CHECK(test::hexBytes(hostWire.sent[2].bytes) == kSeal);
    for (const auto& message : hostWire.sent) {
        REQUIRE(guest.receive(99, message.bytes));
    }
    REQUIRE(guest.flush(guestWire));
    REQUIRE(guestWire.sent.size() == 1);
    constexpr std::string_view kAcknowledge = "47445053"  // magic
                                              "01020000"; // version, acknowledge, seat, zero
    CHECK(test::hexBytes(guestWire.sent[0].bytes) == kAcknowledge);
    REQUIRE(host.receive(2, guestWire.sent[0].bytes));
    CHECK(host.complete());
}
TEST_CASE("party bootstrap rejects stolen seats altered local selections and premature seals",
          "[netplay][party-bootstrap]") {
    const MatchOwners owners{1, 2, 0, 0};
    PartyBootstrap host;
    PartyBootstrap guest;
    REQUIRE(host.open(1, owners, std::array{MatchLink{2, 42}}, selections(owners, 1)));
    REQUIRE(guest.open(2, owners, std::array{MatchLink{1, 99}}, selections(owners, 2)));
    Wire wire;
    REQUIRE(guest.flush(wire));
    REQUIRE(wire.sent.size() == 1);
    const auto packet = wire.sent.front().bytes;
    SECTION("unadmitted connection") {
        CHECK_FALSE(host.receive(99, packet));
    }
    SECTION("foreign seat") {
        auto bad = packet;
        bad[6] = 0;
        CHECK_FALSE(host.receive(42, bad));
    }
    SECTION("unused seat") {
        auto bad = packet;
        bad[6] = 2;
        CHECK_FALSE(host.receive(42, bad));
    }
    SECTION("out of bounds seat") {
        auto bad = packet;
        bad[6] = 4;
        CHECK_FALSE(host.receive(42, bad));
    }
    SECTION("changed profile replay") {
        REQUIRE(host.receive(42, packet));
        REQUIRE(host.receive(42, packet));
        auto bad = packet;
        bad[16] = 'X';
        CHECK_FALSE(host.receive(42, bad));
    }
    SECTION("host cannot replace local choice") {
        auto bad = packet;
        bad[16] = 'X';
        CHECK_FALSE(guest.receive(99, bad));
    }
    SECTION("guest cannot seal host") {
        CHECK_FALSE(host.receive(42, std::array<u8, 8>{'G', 'D', 'P', 'S', 1, 1, 0, 0}));
    }
    SECTION("host cannot seal incomplete party") {
        CHECK_FALSE(guest.receive(99, std::array<u8, 8>{'G', 'D', 'P', 'S', 1, 1, 0, 0}));
    }
    SECTION("acknowledgement before profiles") {
        CHECK_FALSE(host.receive(42, std::array<u8, 8>{'G', 'D', 'P', 'S', 1, 2, 0, 0}));
    }
    SECTION("unsupported or malformed envelope") {
        for (const usize offset : {0U, 4U, 5U, 7U}) {
            auto bad = packet;
            bad[offset] = 255;
            CHECK_FALSE(host.receive(42, bad));
        }
        for (usize size = 0; size < packet.size(); ++size) {
            CHECK_FALSE(host.receive(42, std::span(packet).first(size)));
        }
    }
    CHECK_FALSE(host.complete());
    CHECK_FALSE(guest.complete());
}
TEST_CASE("party admission and reliable delivery failures cannot publish a partial party",
          "[netplay][party-bootstrap]") {
    const MatchOwners owners{1, 2, 0, 0};
    PartyBootstrap guest;
    auto local = selections(owners, 2);
    CHECK_FALSE(guest.open(0, owners, {}, local));
    CHECK_FALSE(guest.open(2, owners, {}, local));
    CHECK_FALSE(guest.open(2, owners, std::array{MatchLink{1, 99}}, {}));
    auto bad = local;
    bad[0].name = "TOO LONG";
    CHECK_FALSE(guest.open(2, owners, std::array{MatchLink{1, 99}}, bad));
    CHECK_FALSE(guest.opened());
    REQUIRE(guest.open(2, owners, std::array{MatchLink{1, 99}}, local));
    Wire wire;
    wire.budget = 0;
    for (usize i = 0; i < 100; ++i) {
        REQUIRE(guest.flush(wire));
    }
    CHECK(wire.sent.empty());
    wire.result = PacketTransport::SendResult::Disconnected;
    CHECK_FALSE(guest.flush(wire));
    wire.result = PacketTransport::SendResult::Sent;
    wire.budget = 100;
    REQUIRE(guest.flush(wire));
    REQUIRE(wire.sent.size() == 1);
    CHECK_FALSE(guest.complete());
}
} // namespace
