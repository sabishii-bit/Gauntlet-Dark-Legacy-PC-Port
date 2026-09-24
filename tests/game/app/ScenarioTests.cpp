
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/players/Progression.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("mountain creature scenarios provide unsaved level 99 green knights",
          "[game][scenario][mountain-creatures]") {
    for (const auto* name : {"golem", "gargoyle"}) {
        const auto file = test::dataDirectory().parent_path() / "tests/scenarios" /
                          (std::string{"level-b5-mountain-"} + name + ".json");
        const auto scenario = Scenario::load(file);
        REQUIRE(scenario.level == "B5");
        REQUIRE(scenario.party.size() == 1);
        const auto party = scenario.partyMembers();
        REQUIRE(party[0].save.character == classIndexOf("KNI").value());
        REQUIRE(party[0].save.color == colorIndexOf("GRE").value());
        REQUIRE(experienceLevel(party[0].save.experience()) == 99);
        REQUIRE(party[0].save.health() == 10400);
        REQUIRE_FALSE(party[0].slot.has_value());
        REQUIRE(scenario.tower.position.has_value());
        REQUIRE(scenario.tower.welcome == false);
    }
}

TEST_CASE("mountain creature scenario starts have walkable clearance near their placed enemy",
          "[game][scenario][mountain-creatures][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELB5/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto ref = levels.byName("B5");
    REQUIRE(ref.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    for (const auto* name : {"golem", "gargoyle"}) {
        const auto file = test::dataDirectory().parent_path() / "tests/scenarios" /
                          (std::string{"level-b5-mountain-"} + name + ".json");
        const auto scenario = Scenario::load(file);
        const auto position = *scenario.tower.position;
        const auto floor = world.collision().floorAt(position, 2, 6);
        INFO(name);
        REQUIRE(floor.has_value());
        INFO("floor height: " << floor->y);
        const auto resolved =
            world.collision().resolveWalls(position, 1.5f, floor->y + 0.1f, floor->y + 6);
        REQUIRE(glm::length(resolved - position) < 0.01f);
        bool found = false;
        for (const auto& instance : world.layout().itemInstances()) {
            if (instance.info < 0 || instance.minPlayers != 1) {
                continue;
            }
            const auto& info = world.layout().itemInfos()[static_cast<usize>(instance.info)];
            const std::string_view wanted = std::string_view{name} == "golem" ? "GOLEM" : "GAR";
            if (info.type == ItemInfo::kPlacedEnemy && info.name == wanted &&
                glm::length(instance.position - position) < 25) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("a shop scenario stages results without awarding their totals twice",
          "[game][scenario][shop]") {
    const auto scenario = Scenario::fromJson(R"({"screen":"shop","party":[
        {"player":3,"gold":5000,"results":{"gold":800,"kills":12,"experience":300}}]})");
    REQUIRE(scenario.afterLevel);
    REQUIRE(scenario.results.size() == 1);
    REQUIRE(scenario.results[0].player == 3);
    REQUIRE(scenario.results[0].totals[0] == 800);
    REQUIRE(scenario.partyMembers()[0].save.gold == 5000);
    REQUIRE(scenario.partyMembers()[0].save.experience() == 0);
}

TEST_CASE("a scenario can stage pending tower promotions", "[game][scenario][promotion]") {
    const auto scenario = Scenario::fromJson(R"({"party":[
        {"level":30,"promotedLevel":29},{"level":80,"promotedLevel":79}],"arrivalWorld":7})");
    REQUIRE(scenario.tower.arrivalWorld == 7);
    const auto party = scenario.partyMembers();
    REQUIRE(party[0].save.progress().appearanceLevel() == 29);
    REQUIRE(party[1].save.progress().appearanceLevel() == 79);
    REQUIRE(party[0].save.progress().promotionPending());
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party":[{"level":30,"promotedLevel":80}]})"),
                      FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party":[{"promotedLevel":0}]})"), FormatError);
}

TEST_CASE("a scenario describes a party, where it stands and whether it is welcomed",
          "[game][scenario]") {
    const Scenario scenario = Scenario::fromJson(R"({
  "screen": "tower",
  "party": [
    {"player": 2, "class": "val", "color": "red", "name": "Kim", "level": 3, "crystals": [0, 5],
     "gold": 120, "health": 250, "keys": 2, "slot": 5, "turbo": 45, "potions": [1, 4],
     "powerups": [{"kind": 5, "flags": 524288}, {"kind": 7, "charge": 2.5, "strength": 60, "active": false}],
     "legends": [7, 2]},
    {"class": "WAR"}
  ],
  "position": [19.3, -2, -60],
  "yaw": 1.5,
  "welcome": false,
  "level": "G1",
  "items": [{"name": "KEY", "position": [1, 2, 3]}, {"name": "HAM", "position": [4, 5, 6]}]
})");
    REQUIRE(scenario.level == "G1");
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
    REQUIRE(members[0].slot == std::optional<usize>{5}); // kept in a slot when it names one
    REQUIRE(members[0].turbo == 45.0f);
    REQUIRE(members[1].turbo == 0.0f);
    REQUIRE(members[0].save.progress().inventory.nextPotion() == 4);
    REQUIRE(members[0].save.progress().inventory.powerupCount() == 2);
    REQUIRE(members[0].save.progress().inventory.powerup(5, 0x80000)->strength == 30.0f);
    REQUIRE(members[0].save.progress().inventory.powerups[1].charge == 2.5f);
    REQUIRE_FALSE(members[0].save.progress().inventory.powerups[1].on);
    REQUIRE(members[0].save.progress().relics.hasLegend(7)); // the town's book, the mountain's axe
    REQUIRE(members[0].save.progress().relics.hasLegend(2));
    REQUIRE_FALSE(members[0].save.progress().relics.hasLegend(1));
    REQUIRE_FALSE(members[1].save.progress().relics.hasLegend(7));
    REQUIRE(members[1].save.progress().inventory == Inventory{});
    REQUIRE(members[1].save.health() == 500);
    // The second takes the defaults: the next player, yellow, level one, named TEST.
    REQUIRE(members[1].player == 1);
    REQUIRE_FALSE(members[1].slot.has_value());
    REQUIRE(members[1].save.name == "TEST");
    REQUIRE(members[1].save.color == 0);
    REQUIRE(members[1].save.experience() == 0);

    // Nothing but the party is needed.
    const Scenario bare = Scenario::fromJson(R"({"party": [{}]})");
    REQUIRE_FALSE(bare.tower.position.has_value());
    REQUIRE_FALSE(bare.tower.yaw.has_value());
    REQUIRE_FALSE(bare.tower.welcome.has_value());
    REQUIRE(bare.tower.items.empty());
    REQUIRE(bare.level.empty()); // the tower
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"keys": 12}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"legends": [0]}]})"), FormatError);
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"legends": [16]}]})"), FormatError);
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
    REQUIRE_THROWS_AS(Scenario::fromJson(R"({"party": [{"name": "TOOLONGNAME"}]})"), FormatError);
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
