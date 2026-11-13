#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"

#include "FakeGameServices.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Relics.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"
#include "game/tower/TowerGateways.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<TowerGateways>);
static_assert(std::has_virtual_destructor_v<TowerGateways>);

TEST_CASE("the worlds past Sumner's gateways come as offered, every ask a look of its own",
          "[game][tower][gateways]") {
    test::FixedGateways fixed;
    TowerGateways& gateways = fixed;
    CHECK(gateways.worlds().empty()); // an installation with nothing past them
    CHECK(fixed.asks == 1);
    fixed.offered = {{3, "a", true}, {1, "b", false}};
    const auto worlds = gateways.worlds();
    CHECK(fixed.asks == 2);
    // In the order offered, ids and names as their expansions give them, usable or not.
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0] == GatewayWorld{3, "a", true});
    CHECK(worlds[1] == GatewayWorld{1, "b", false});
    CHECK(worlds[1] != GatewayWorld{1, "b", true});
    CHECK(GatewayWorld{} == GatewayWorld{0, "", false}); // none usable until found
}

TEST_CASE("Sumner lists the same places in every tower, greying what cannot be chosen",
          "[game][tower][gateways]") {
    constexpr u8 kFirst = 0;
    constexpr u8 kSecond = 1;
    const GatewayDestination home;
    const GatewayDestination first{kFirst};
    const GatewayDestination second{kSecond};
    CHECK(home != first);
    CHECK(first != second);
    // Which worlds this installation can open: none, either one, or both.
    const auto offered = [](bool firstUsable, bool secondUsable) {
        return std::vector<GatewayWorld>{{kFirst, "first", firstUsable},
                                         {kSecond, "second", secondUsable}};
    };
    const auto enabled = [](const std::vector<GatewayChoice>& choices) {
        std::vector<bool> result;
        result.reserve(choices.size());
        for (const GatewayChoice& choice : choices) {
            result.push_back(choice.enabled);
        }
        return result;
    };
    for (const bool firstUsable : {false, true}) {
        for (const bool secondUsable : {false, true}) {
            CAPTURE(firstUsable, secondUsable);
            const auto worlds = offered(firstUsable, secondUsable);
            for (const GatewayDestination& here : {home, first, second}) {
                CAPTURE(here.world.value_or(0xFF));
                // The campaign's own tower first, then the worlds in the order offered.
                const auto choices = gatewayChoices(worlds, here);
                REQUIRE(choices.size() == 3);
                CHECK(choices[0].destination == home);
                CHECK(choices[0].label == kGatewayHomeText);
                CHECK(choices[1].destination == first);
                CHECK(choices[1].label == "first");
                CHECK(choices[2].destination == second);
                CHECK(choices[2].label == "second");
                // Greyed: a world this installation cannot open, and where the party stands.
                CHECK(enabled(choices) == std::vector<bool>{here != home,
                                                            firstUsable && here != first,
                                                            secondUsable && here != second});
            }
        }
    }
    // With no world offered past the gateways, the list holds the campaign's own tower alone.
    CHECK(gatewayChoices({}, home) ==
          std::vector<GatewayChoice>{{home, std::string(kGatewayHomeText), false}});
    CHECK(gatewayChoices({}, first) ==
          std::vector<GatewayChoice>{{home, std::string(kGatewayHomeText), true}});
}

TEST_CASE("a world whose tower would not open is greyed, and no other", "[game][tower][gateways]") {
    std::vector<GatewayWorld> worlds{{0, "first", true}, {1, "second", true}, {2, "third", false}};
    greyUnopened(worlds, {});
    CHECK(worlds ==
          std::vector<GatewayWorld>{{0, "first", true}, {1, "second", true}, {2, "third", false}});
    const std::vector<u8> unopened{1, 2, 7};
    greyUnopened(worlds, unopened);
    CHECK(worlds ==
          std::vector<GatewayWorld>{{0, "first", true}, {1, "second", false}, {2, "third", false}});
    // So Sumner's list greys it as it greys a world this installation cannot open.
    CHECK(gatewayChoices(worlds, {})[2].enabled == false);
    CHECK(gatewayChoices(worlds, {})[1].enabled == true);
}

TEST_CASE("Sumner's gateway row and his own tower read as the user worded them",
          "[game][tower][gateways]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    CHECK(strings.get(kGatewayTopicText) == "Legacy of Legends");
    CHECK(strings.get(kGatewayHomeText) == "The Dark Legacy");
    // The row answers to no topic of the original's (39 to 42, OPTMENU 0x8011F804's items).
    for (const s32 retail : {39, 40, 41, 42}) {
        CHECK(kGatewayTopic != retail);
    }
}

TEST_CASE("a character has beaten Garm while the class it plays holds his piece",
          "[game][tower][gateways]") {
    // His piece is the battlefield's, eleventh in the tower's order (0x80124D14).
    REQUIRE(LevelRef::orderOf(TowerAccess::kBattlefieldWorld) == 11);
    CharacterSave save;
    CHECK_FALSE(beatenGarm(save));
    // Every other boss's piece, the ten a Sumner starts with, is not his.
    save.progress().relics.shards = Relics::kAllShards;
    CHECK_FALSE(beatenGarm(save));
    save.progress().relics.addShard(11);
    CHECK(beatenGarm(save));
    // Another class keeps its own pieces: playing it, the character has not beaten him.
    save.selectClass(2);
    CHECK_FALSE(beatenGarm(save));
    save.selectClass(0);
    CHECK(beatenGarm(save));
}
} // namespace
