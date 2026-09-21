#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/players/Progression.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a scenario describes a party, where it stands and whether it is welcomed",
          "[game][scenario]") {
    const Scenario scenario = Scenario::fromJson(R"({
  "screen": "tower",
  "party": [
    {"player": 2, "class": "val", "color": "red", "name": "Kim", "level": 3, "crystals": [0, 5],
     "gold": 120, "health": 250, "keys": 2, "potions": [1, 4]},
    {"class": "WAR"}
  ],
  "position": [19.3, -2, -60],
  "yaw": 1.5,
  "welcome": false,
  "items": [{"name": "KEY", "position": [1, 2, 3]}, {"name": "HAM", "position": [4, 5, 6]}]
})");
    REQUIRE(scenario.tower.items.size() == 2);
    REQUIRE(scenario.tower.items[0].name == "KEY");
    REQUIRE(scenario.tower.items[1].position == Vec3{4.0f, 5.0f, 6.0f});
    REQUIRE(scenario.party.size() == 2);
    REQUIRE(scenario.tower.position == Vec3{19.3f, -2.0f, -60.0f});
    REQUIRE(scenario.tower.yaw == 1.5f);
    REQUIRE(scenario.tower.welcome == false);
    const std::vector<PartyMember> members = scenario.partyMembers();
    REQUIRE(members.size() == 2);
    REQUIRE(members[0].player == 2);
    REQUIRE(members[0].save.name == "Kim");
    REQUIRE(members[0].save.character == classIndexOf("VAL").value());
    REQUIRE(members[0].save.color == colorIndexOf("RED").value());
    REQUIRE(members[0].save.experience() == levelExperience(3));
    REQUIRE(members[0].save.progress().crystals[1] == 5);
    REQUIRE(members[0].save.progress().crystals[0] == 0);
    REQUIRE(members[0].save.gold == 120);
    REQUIRE(members[0].save.health() == 250);
    REQUIRE(members[0].save.progress().inventory.keys == 2);
    REQUIRE(members[0].save.progress().inventory.nextPotion() == 4);
    REQUIRE(members[1].save.progress().inventory == Inventory{});
    REQUIRE(members[1].save.health() == 500);
    // The second takes the defaults: the next player, yellow, level one, named TEST.
    REQUIRE(members[1].player == 1);
    REQUIRE(members[1].save.name == "TEST");
    REQUIRE(members[1].save.color == 0);
    REQUIRE(members[1].save.experience() == 0);

    // Nothing but the party is needed.
    const Scenario bare = Scenario::fromJson(R"({"party": [{}]})");
    REQUIRE_FALSE(bare.tower.position.has_value());
    REQUIRE_FALSE(bare.tower.yaw.has_value());
    REQUIRE_FALSE(bare.tower.welcome.has_value());
    REQUIRE(bare.tower.items.empty());
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"keys": 12}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{}], "items": [{"name": "KEY"}]})"),
                      FormatError);
    REQUIRE(bare.partyMembers()[0].save.character == 0);
}

TEST_CASE("a scenario with an unknown class, colour, screen or no party is refused",
          "[game][scenario]") {
    REQUIRE_THROWS_AS(Scenario::fromJson("{nope"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": []})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"screen": "title", "party": [{}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"class": "XYZ"}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"color": "PINK"}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"player": 9}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"name": "TOOLONGNAME"}]})"),
                      FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{}], "position": [1, 2]})"), FormatError);
}

TEST_CASE("a scenario loads from a file", "[game][scenario]") {
    const auto dir = test::scratchDirectory("scenario-file");
    writeTextFile(dir / "start.json", R"({"party": [{"class": "ARC", "name": "AB"}]})");
    const Scenario scenario = Scenario::load(dir / "start.json");
    REQUIRE(scenario.party[0].classCode == "ARC");
    REQUIRE_THROWS(Scenario::load(dir / "missing.json"));
}

} // namespace
