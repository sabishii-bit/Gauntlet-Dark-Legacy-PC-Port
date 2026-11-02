#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/rollback/BitSet8.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a bit set is the one byte it carries", "[game][bit-set]") {
    STATIC_REQUIRE(sizeof(PeerSet) == 1);
    STATIC_REQUIRE(sizeof(SeatSet) == 1);
    STATIC_REQUIRE(std::is_trivially_copyable_v<PeerSet>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<SeatSet>);
    STATIC_REQUIRE_FALSE(std::is_same_v<PeerSet, SeatSet>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<u8, SeatSet>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<SeatSet, u8>);
    STATIC_REQUIRE(std::is_same_v<decltype(SeatSet{}.bits()), u8>);
    for (u32 bits = 0; bits < 256; ++bits) {
        CHECK(SeatSet::fromBits(static_cast<u8>(bits)).bits() == bits);
    }
}

TEST_CASE("a bit set holds the indices below eight", "[game][bit-set]") {
    constexpr SeatSet kEmpty;
    STATIC_REQUIRE(kEmpty.empty());
    STATIC_REQUIRE(kEmpty.count() == 0);
    STATIC_REQUIRE(kEmpty.bits() == 0);

    SeatSet seats;
    seats.insert(0);
    seats.insert(2);
    seats.insert(2);
    CHECK(seats.bits() == 0b0101);
    CHECK(seats.count() == 2);
    CHECK_FALSE(seats.empty());
    CHECK(seats.contains(0));
    CHECK_FALSE(seats.contains(1));
    CHECK(seats.contains(2));
    seats.erase(0);
    seats.erase(3);
    CHECK(seats.bits() == 0b0100);

    PeerSet peers;
    peers.insert(7);
    CHECK(peers.bits() == 0x80);
    CHECK(peers.contains(7));
    peers.insert(8);
    peers.insert(31);
    CHECK(peers.bits() == 0x80);
    CHECK_FALSE(peers.contains(8));
    peers.erase(8);
    CHECK(peers.bits() == 0x80);
    STATIC_REQUIRE(PeerSet::of(3).bits() == 0b1000);
    STATIC_REQUIRE(PeerSet::of(8).empty());
}

TEST_CASE("bit sets join, part and compare by their members", "[game][bit-set]") {
    constexpr auto kGuests = PeerSet::fromBits(0b11100);
    CHECK(kGuests.includes(PeerSet::of(2)));
    CHECK(kGuests.includes(PeerSet::fromBits(0b01100)));
    CHECK(kGuests.includes(PeerSet{}));
    CHECK_FALSE(kGuests.includes(PeerSet::of(1)));
    CHECK_FALSE(kGuests.includes(PeerSet::fromBits(0b10010)));

    auto votes = SeatSet::fromBits(0b0001);
    votes.insert(SeatSet::fromBits(0b0110));
    CHECK(votes == SeatSet::fromBits(0b0111));
    votes.erase(SeatSet::fromBits(0b1010));
    CHECK(votes == SeatSet::fromBits(0b0101));
    CHECK(votes != SeatSet::fromBits(0b0100));
}

TEST_CASE("a bit set digests as the byte it is", "[game][bit-set]") {
    for (const u8 bits : {u8{0}, u8{0b0101}, u8{0b11100}, u8{0xFF}}) {
        NativeStateDigest set;
        set.value(SeatSet::fromBits(bits));
        NativeStateDigest byte;
        byte.value(bits);
        CHECK(set.finish() == byte.finish());
    }
}

} // namespace
