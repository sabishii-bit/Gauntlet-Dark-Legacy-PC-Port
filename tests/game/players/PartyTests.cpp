#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/players/Party.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a party is written back to the slots its members came from", "[game][players][party]") {
    const auto dir = test::scratchDirectory("party-saves");
    std::filesystem::remove_all(dir);
    SaveSlots slots;
    REQUIRE(slots.open(dir, 4));

    PartyMember kept;
    kept.player = 0;
    kept.save.name = "KEPT";
    kept.save.gold = 750;
    kept.save.progress().inventory.keys = 3;
    kept.slot = 2;
    PartyMember passing; // never saved: it has no slot
    passing.player = 1;
    passing.save.name = "GONE";
    PartyMember astray; // a slot the folder does not have
    astray.player = 2;
    astray.save.name = "LOST";
    astray.slot = 9;
    const std::vector<PartyMember> party{kept, passing, astray};
    REQUIRE(saveParty(slots, party) == 1);

    SaveSlots reopened;
    REQUIRE(reopened.open(dir, 4));
    CharacterSave read;
    REQUIRE(reopened.load(2, read));
    REQUIRE(read.name == "KEPT");
    REQUIRE(read.gold == 750);
    REQUIRE(read.progress().inventory.keys == 3);
    REQUIRE_FALSE(reopened.load(0, read));
    REQUIRE_FALSE(reopened.load(1, read));
    REQUIRE(saveParty(slots, {}) == 0);
}

TEST_CASE("a level beaten is recorded by everyone standing, never by the fallen",
          "[game][players][party][level-record]") {
    std::vector<PartyMember> party(3);
    party[1].fallen = true;
    party[2].save.character = 3;
    REQUIRE(recordLevelBeaten(party, 7, 0, 8, 0) == 2);
    REQUIRE(party[0].save.progress().levels.hasBeaten(7, 0));
    REQUIRE(party[0].save.progress().levels.runeLevels[0] == (1U << 7U));
    REQUIRE_FALSE(party[1].save.progress().levels.hasBeaten(7, 0));
    // Into the class each is playing.
    REQUIRE(party[2].save.classes[3].levels.hasBeaten(7, 0));
    REQUIRE_FALSE(party[2].save.classes[0].levels.hasBeaten(7, 0));
    REQUIRE(recordLevelBeaten({}, 7, 0, 0, 0) == 0);
}

} // namespace
