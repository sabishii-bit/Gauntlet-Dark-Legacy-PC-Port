#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"

#include "game/console/ConsoleListings.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/console/FixtureRequest.h"
#include "game/console/LevelListingLines.h"
#include "game/scenes/FixtureConsoleCommands.h"
#include "game/scenes/LevelListings.h"
#include "game/scenes/PlayedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::joinedLines;
using test::printed;
using Words = std::vector<std::string>;

/** What the stand-in for the Legends maps adds to list maps. */
constexpr std::string_view kLegendsLine = "n64: castle1 tower2";

/** What list answers words it cannot read, with the level's listings alone registered. */
constexpr std::string_view kLevelUsage =
    "list maps              every level, by realm\n"
    "list items [map]       what get knows, then a level's pickups\n"
    "list inventory         what your character is and carries\n"
    "list enemies [map]     the swarm's kinds, then a level's roster and generators\n"
    "list critters [map]    the great ones, then those a level places\n"
    "list generators [map]  a level's generators\n"
    "list objects [map]     a level's objects\n"
    "list switches [map]    a level's switches, for activate";

/** The level's listings over a level of the test's own, with a stand-in for the Legends maps
 * after them, as the expansion adds its own. */
struct Listings {
    std::unique_ptr<test::PlayedLevel> level;
    ConsoleListings listings;
    DevConsole console;

    explicit Listings(std::unique_ptr<test::PlayedLevel> played) : level(std::move(played)) {
        registerLevelListings(console.host(), listings,
                              {.viewed = [this] { return level->openWorld(); },
                               .levels = level->catalog,
                               .unpackedRoot = level->root,
                               .save = [this] { return level->playerSave(); }});
        listings.add(std::make_unique<LambdaListing>(
            "maps", std::nullopt,
            [](ConsoleListing::Arguments /*rest*/) { return Words{std::string{kLegendsLine}}; }));
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("with nothing in play the level's listings answer in their own words", "[dev-console]") {
    Listings fixture{std::make_unique<test::PlayedLevel>()};
    CHECK(fixture.say("list") == test::shownAs(kLevelUsage));
    CHECK(fixture.say("list nonsense") == test::shownAs(kLevelUsage));
    CHECK(fixture.say("list maps") == "n64: castle1 tower2");
    CHECK(fixture.say("list inventory") == "Nobody is in.");
    CHECK(fixture.say("list Inventory") == "Nobody is in.");
    CHECK(fixture.say("list switches") == "Nothing is in play.");
    CHECK(fixture.say("list objects") == "Nothing is in play.");
    CHECK(fixture.say("list generators") == "Nothing is in play.");
    CHECK(fixture.say("list items G1") == "No map named G1; list maps names them");
    // The map named after a listing is read in any case.
    CHECK(fixture.say("list objects TOWER") == "L1 is not unpacked");
    CHECK(fixture.say("list OBJECTS tower") == "L1 is not unpacked");
    CHECK(fixture.say("list items") ==
          printed(joinedLines({itemLines(), {"Nothing is in play."}})));
    CHECK(fixture.say("list ITEMS") == fixture.say("list items"));
    CHECK(fixture.say("list enemies") ==
          "Enemies (spawn <enemy> [tier 1-3] [count]): SCO, TRO, DEM, RAT, GRU, KNI, SNA, SOR, "
          "MUM, SPI, LIZ, TRE, MAG, ZOM, PLA, WOL,\n"
          "ICE, WRM, DOG, SKE, GHO, ACI, HAN, IMP, WAR, SKY, WIND, GRM, DEATH, IT\n"
          "Nothing is in play.");
    CHECK(fixture.say("list critters") ==
          "Critters (spawn <critter>): golem, general, gargoyle [eagl|lion|serp]\n"
          "Nothing is in play.");
    CHECK(fixture.listings.words() == Words{"maps", "items", "inventory", "enemies", "critters",
                                            "generators", "objects", "switches"});
}

TEST_CASE("the level's listings answer in a level in their own words", "[dev-console][assets]") {
    Listings fixture{std::make_unique<test::PlayedLevel>("G2")};
    const test::PlayedLevel& level = *fixture.level;
    const auto g1 = level.catalog.byName("G1");
    REQUIRE(g1);
    WorldLayout fieldsLayout;
    REQUIRE(fieldsLayout.load(level.root / g1->directory));
    CHECK(
        fixture.say("list maps") ==
        "T test (0): T1 TEST1, T2 TEST2, T3 TEST3\n"
        "A castle (1): A1 Courtyard, A6 Dungeon, A2 Barracks, A3 Armory, A4 Treasury, A5 Chimera\n"
        "B mount (2): B1 Valley, B2 Peak, B3 Cliff, B4 Cave, B5 Cavern, B6 Dragon\n"
        "C desert (3): C1 Ruins, C2 Temple, C3 Pyramid, C4 Tombs, C5 Genie\n"
        "D forest (4): D1 Swamp, D2 Roots, D3 Tree, D4 Branches, D5 Spider\n"
        "E temple (5): E1 Temple, E2 Altar\n"
        "F hell (6): F1 Gates, F2 Throne\n"
        "G town (7): G1 Fields, G2 Town, G3 Cemetary, G4 Mausoleum, G5 Lich\n"
        "H battle (8): H1 Trenches, H2 Towers, H3 Fortress, H4 Citadel\n"
        "I ice (9): I1 Docks, I2 Camp, I3 Mine, I4 Fissure, I5 Yeti\n"
        "J dream (10): J1 Carnival, J2 Grounds, J3 House, J6 Nightmare, J4 Illusion, J5 Wraith\n"
        "K sky (11): K2 Spire, K3 Docks, K4 Shipyard, K1 Mothership, K5 Fiend\n"
        "S secret (12): S1 Lab, S2 Psychedelic, S3 Space, S4 Gauntlet, S5 Trenches, S6 Docks, S7 "
        "Henhouse, S8 Crops, S9 Cloud9\n"
        "L tower (13): L1 Tower, L2 Tower\n"
        "n64: castle1 tower2");
    CHECK(fixture.say("list inventory") ==
          "TESTER the WAR at level 1 (0 experience), health 4000/500, 0 gold\n"
          "Keys 0/9, potions 0/9\n"
          "No powerups\n"
          "Runestones 0/13, boss shards 0, legend items 0, gargoyle pieces 0/12 0/20 0/28\n"
          "Crystals: none");
    CHECK(fixture.say("list enemies") ==
          "Enemies (spawn <enemy> [tier 1-3] [count]): SCO, TRO, DEM, RAT, GRU, KNI, SNA, SOR, "
          "MUM, SPI, LIZ, TRE, MAG, ZOM, PLA, WOL,\n"
          "ICE, WRM, DOG, SKE, GHO, ACI, HAN, IMP, WAR, SKY, WIND, GRM, DEATH, IT\n"
          "In G2:\n"
          "Roster: medium ZOM, small MAG, GOLEM, GENERAL, GAR\n"
          "Generators: GRU (tier 3); KNI x72 (tier 1, 2, 3); RAT x41 (tier 1, 2)\n"
          "Placed enemies: DEATH x3, GRU, KNI x45");
    CHECK(fixture.say("list enemies G1") ==
          "In G1:\n"
          "Roster: medium ZOM, small MAG, GOLEM, GENERAL, GAR\n"
          "Generators: GRU x102 (tier 1, 2, 3); RAT x15 (tier 1)\n"
          "Placed enemies: DEATH x2, GHO, GRU x45");
    CHECK(fixture.say("list critters") ==
          "Critters (spawn <critter>): golem, general, gargoyle [eagl|lion|serp]\n"
          "In G2:\n"
          "Critters placed: GAR, GENERAL x4, GOLEM x2");
    CHECK(fixture.say("list generators") ==
          "In G2:\n"
          "Generators: GRU (tier 3); KNI x72 (tier 1, 2, 3); RAT x41 (tier 1, 2)\n"
          "spawn generator <enemy> [tier] stands another");
    CHECK(fixture.say("list items Z9") == "No map named Z9; list maps names them");
    // The longest listings are the listing lines themselves, under the level's name.
    const WorldLayout& layout = level.world.layout();
    CHECK(fixture.say("list switches") ==
          printed(joinedLines({{"In G2:"}, switchLines(switchesInPlay(level.world))})));
    CHECK(fixture.say("list switches G1") ==
          printed(joinedLines({{"In G1:"}, switchLines(switchesOf(fieldsLayout))})));
    CHECK(fixture.say("list items G1") ==
          printed(joinedLines({{"In G1:"}, pickupLines(fieldsLayout)})));
    CHECK(fixture.say("list items g1") == fixture.say("list items G1"));
    CHECK(fixture.say("list items") ==
          printed(joinedLines({itemLines(), {"In G2:"}, pickupLines(layout)})));
    CHECK(fixture.say("list objects") == printed(joinedLines({{"In G2:"}, objectLines(layout)})));
}
