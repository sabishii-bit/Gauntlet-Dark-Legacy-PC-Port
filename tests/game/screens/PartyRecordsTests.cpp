#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/screens/PartyRecords.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::array<PlayerRuntime, 2> twoPlayers() {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(2, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    players[1].actor.spawn(3, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    players[1].slot = 4;
    for (PlayerRuntime& runtime : players) {
        runtime.entrySave = runtime.actor.save();
    }
    return players;
}

TEST_CASE("experience won goes to its player by identity and feeds the meter on a kill",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    PartyRecords::award(players, 3, 100, true, nullptr);
    CHECK(players[0].actor.save().experience() == 0);
    CHECK(players[1].actor.save().experience() == 100);
    CHECK(players[1].levelKills == 1);
    CHECK(players[1].turbo.held() == Approx(TurboMeter::kPerExperience * 100.0f));
    // Won otherwise, it feeds nothing; a kill worth nothing still counts.
    PartyRecords::award(players, 3, 40, false, nullptr);
    CHECK(players[1].actor.save().experience() == 140);
    CHECK(players[1].levelKills == 1);
    PartyRecords::award(players, 2, 0, true, nullptr);
    CHECK(players[0].levelKills == 1);
    // The fallen win nothing.
    players[0].life = PlayerLife::Dying;
    PartyRecords::award(players, 2, 100, true, nullptr);
    CHECK(players[0].actor.save().experience() == 0);
    CHECK(players[0].levelKills == 1);
}

TEST_CASE("the party goes on with what it gathered, the fallen as they came, the departed not",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    players[0].actor.save().gold = 50;
    players[0].actor.save().helpSeen = {9};
    players[0].helpHeard = {12};
    players[1].actor.save().gold = 70;
    players[1].actor.save().helpSeen = {4};
    players[1].life = PlayerLife::InTower;
    std::vector<PartyMember> members = PartyRecords::members(players);
    REQUIRE(members.size() == 2);
    CHECK(members[0].player == 2);
    CHECK(members[0].save.gold == 50);
    CHECK(members[0].helpHeard == std::vector<s32>{12});
    CHECK_FALSE(members[0].fallen);
    CHECK(members[1].save.gold == 0); // as it came in
    CHECK(members[1].save.helpSeen == std::vector<s32>{4});
    CHECK(members[1].fallen);
    CHECK(members[1].slot == std::optional<usize>{4});
    const std::vector<LevelResults> results = PartyRecords::results(players);
    REQUIRE(results.size() == 1);
    CHECK(results[0].player == 2);
    CHECK(results[0].totals[0] == 50);

    // Giving the level up, everyone is as they came in but keeps what they were taught.
    const std::vector<PartyMember> left = PartyRecords::abandoned(players, members);
    REQUIRE(left.size() == 2);
    CHECK(left[0].save.gold == 0);
    CHECK(left[0].save.helpSeen == std::vector<s32>{9});
    CHECK_FALSE(left[1].fallen);

    players[1].departed = true;
    CHECK(PartyRecords::members(players).size() == 1);
}

} // namespace
