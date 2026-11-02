#include <algorithm>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/rollback/PartyCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("travel profile packets are bounded and reject malformed envelopes",
          "[netplay][party-checkpoint]") {
    PartyCheckpoint state;
    state.epoch = 0x123456789abcdefULL;
    state.seat = 3;
    state.profile.name = "PLAYER";
    state.profile.gold = 3200;
    state.profile.progress.inventory.keys = 9;
    state.profile.progress.inventory.potions = {1, 2, 3};
    const auto bytes = PartyCheckpointPacket::encode(state);
    REQUIRE(bytes);
    CHECK(bytes->size() == PartyCheckpointPacket::kMinBytes);
    CHECK(bytes->size() <= 1200);
    const auto decoded = PartyCheckpointPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->epoch == state.epoch);
    CHECK(decoded->seat == state.seat);
    CHECK(CharacterProfilePacket::encode(decoded->profile) ==
          CharacterProfilePacket::encode(state.profile));
    for (usize length = 0; length < bytes->size(); ++length) {
        CHECK_FALSE(PartyCheckpointPacket::decode(std::span(*bytes).first(length)));
    }
    auto bad = *bytes;
    bad.push_back(0);
    CHECK_FALSE(PartyCheckpointPacket::decode(bad));
    bad = *bytes;
    bad[4] = 1;
    CHECK_FALSE(PartyCheckpointPacket::decode(bad));
    for (const usize offset : {0U, 4U, 5U, 6U, 7U, 16U}) {
        bad = *bytes;
        bad[offset] = 255;
        CHECK_FALSE(PartyCheckpointPacket::decode(bad));
    }
    state.epoch = 0;
    CHECK_FALSE(PartyCheckpointPacket::encode(state));
    state.epoch = 1;
    state.seat = 4;
    CHECK_FALSE(PartyCheckpointPacket::encode(state));
}

TEST_CASE("travel profile packets keep their byte layout", "[netplay][party-checkpoint]") {
    PartyCheckpoint state;
    state.epoch = 0x0123456789abcdefULL;
    state.seat = 3;
    state.profile.name = "PLAYER";
    const auto bytes = PartyCheckpointPacket::encode(state);
    REQUIRE(bytes);
    const auto profile = CharacterProfilePacket::encode(state.profile);
    REQUIRE(profile);
    REQUIRE(bytes->size() == PartyCheckpointPacket::kHeaderBytes + profile->size());
    constexpr std::string_view kHeader = "47445043"          // magic
                                         "02030000"          // version, seat, zero
                                         "efcdab8967452301"; // epoch
    CHECK(test::hexBytes(std::span{*bytes}.first(PartyCheckpointPacket::kHeaderBytes)) == kHeader);
    CHECK(std::ranges::equal(std::span{*bytes}.subspan(PartyCheckpointPacket::kHeaderBytes),
                             *profile));
}

TEST_CASE("travel profiles retain both isolated Legends campaigns",
          "[netplay][party-checkpoint][legends]") {
    PartyCheckpoint state;
    state.epoch = 17;
    state.profile.name = "PLAYER";
    state.profile.progress.legends[0] = {0x8000000000000000ULL, 2, 4, 8, {"mount1", "town4"}};
    state.profile.progress.legends[1] = {16, 32, 64, 128, {"levela1"}};
    const auto bytes = PartyCheckpointPacket::encode(state);
    REQUIRE(bytes);
    CHECK(bytes->size() > PartyCheckpointPacket::kMinBytes);
    CHECK(bytes->size() <= PartyCheckpointPacket::kMaxBytes);
    const auto decoded = PartyCheckpointPacket::decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->profile.progress.legends == state.profile.progress.legends);
}
} // namespace
