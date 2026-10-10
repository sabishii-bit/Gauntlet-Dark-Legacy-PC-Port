#include <catch2/catch_test_macros.hpp>

#include "game/netplay/PartyCheckpoint.h"

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
    CHECK(bytes->size() == PartyCheckpointPacket::kBytes);
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
} // namespace
