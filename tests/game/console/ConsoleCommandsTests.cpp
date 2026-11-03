#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/console/ConsoleCommands.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Args = std::vector<std::string>;

/** What goto answers words it cannot read. */
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]            travels to a level, standing at a point when given\n"
    "goto x y z                    moves the party to that point here\n"
    "goto <legends map> [preview]  walks an installed Legends map, or flies through it\n"
    "goto n64|dreamcast <map>      the same, naming the map's platform\n"
    "goto back                     returns from a Legends map to the level left for it\n"
    "list maps names the levels and the Legends maps installed.";

TEST_CASE("goto takes a map, a point, or a point in a map", "[dev-console][commands]") {
    auto level = ConsoleCommands::parseGoto(Args{"g1"});
    REQUIRE(level.has_value());
    CHECK(level->level == "G1");
    CHECK_FALSE(level->position.has_value());
    auto point = ConsoleCommands::parseGoto(Args{"10", "2.5", "-3"});
    REQUIRE(point.has_value());
    CHECK_FALSE(point->level.has_value());
    REQUIRE(point->position.has_value());
    CHECK(point->position->x == Approx(10.0f));
    CHECK(point->position->y == Approx(2.5f));
    CHECK(point->position->z == Approx(-3.0f));
    auto both = ConsoleCommands::parseGoto(Args{"A1", "1", "2", "3"});
    REQUIRE(both.has_value());
    CHECK(both->level == "A1");
    CHECK(both->position == Vec3{1, 2, 3});
    auto tower = ConsoleCommands::parseGoto(Args{"tower"});
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    const auto none = ConsoleCommands::parseGoto(Args{});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error() == ConsoleCommands::gotoUsage());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"a", "b"}).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"1", "2"}).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"G1", "1", "x", "3"}).has_value());
}

TEST_CASE("a word names a map the way the catalogue does", "[dev-console][commands]") {
    CHECK(ConsoleCommands::looksLikeMap("e2"));
    CHECK(ConsoleCommands::looksLikeMap("S10"));
    CHECK(ConsoleCommands::looksLikeMap("tower"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("E1XLNSNC_CNDL01"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("gru"));
    CHECK_FALSE(ConsoleCommands::looksLikeMap("1"));
}

TEST_CASE("goto takes a Legends map by name or platform, a preview of it, and the way back",
          "[dev-console][commands][legends]") {
    auto named = ConsoleCommands::parseGoto(Args{"n64", "Gauntlet"});
    REQUIRE(named.has_value());
    CHECK(named->legends == "Gauntlet"); // the case kept: it names a folder
    CHECK(named->platform == LegendsPlatform::N64);
    CHECK_FALSE(named->preview);
    CHECK_FALSE(named->back);
    CHECK_FALSE(named->level.has_value());
    CHECK_FALSE(named->position.has_value());
    auto preview = ConsoleCommands::parseGoto(Args{"DC", "castle", "Preview"});
    REQUIRE(preview.has_value());
    CHECK(preview->legends == "castle");
    CHECK(preview->platform == LegendsPlatform::Dreamcast);
    CHECK(preview->preview);
    auto bare = ConsoleCommands::parseGoto(Args{"castle"});
    REQUIRE(bare.has_value());
    CHECK(bare->legends == "castle");
    CHECK_FALSE(bare->platform.has_value());
    CHECK_FALSE(bare->level.has_value());
    auto barePreview = ConsoleCommands::parseGoto(Args{"castle", "preview"});
    REQUIRE(barePreview.has_value());
    CHECK(barePreview->legends == "castle");
    CHECK(barePreview->preview);
    auto back = ConsoleCommands::parseGoto(Args{"back"});
    REQUIRE(back.has_value());
    CHECK(back->back);
    CHECK_FALSE(back->legends.has_value());
    // A level's name is still a level; a platform alone, or with a point, is nothing.
    const auto realmLevel = ConsoleCommands::parseGoto(Args{"e2"});
    REQUIRE(realmLevel.has_value());
    CHECK(realmLevel->level == "E2");
    const auto tower = ConsoleCommands::parseGoto(Args{"Tower"});
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"n64"}).has_value());
    CHECK_FALSE(ConsoleCommands::parseGoto(Args{"n64", "back"}).has_value());
    const auto point = ConsoleCommands::parseGoto(Args{"n64", "1", "2", "3"});
    REQUIRE_FALSE(point.has_value());
    CHECK(point.error() == ConsoleCommands::gotoUsage());
    CHECK(ConsoleCommands::gotoUsage().contains("legends"));
    CHECK(ConsoleCommands::platformOf("Dreamcast") == LegendsPlatform::Dreamcast);
    CHECK(ConsoleCommands::platformOf("N64") == LegendsPlatform::N64);
    CHECK_FALSE(ConsoleCommands::platformOf("gamecube").has_value());
    CHECK(ConsoleCommands::platformName(LegendsPlatform::N64) == "N64");
}

TEST_CASE("goto's Legends forms never reach the shared game; the rest name a level or a point",
          "[dev-console][commands][online-console]") {
    for (const Args& words : {Args{"back"}, Args{"return"}, Args{"n64", "castle"},
                              Args{"castle", "preview"}, Args{"MISSING"}, Args{"lab"}}) {
        CAPTURE(words);
        const auto destination = ConsoleCommands::parseGoto(words);
        REQUIRE(destination.has_value());
        CHECK(destination->offlineOnly());
    }
    for (const Args& words :
         {Args{"g1"}, Args{"tower"}, Args{"10", "2.5", "-3"}, Args{"A1", "1", "2", "3"}}) {
        CAPTURE(words);
        const auto destination = ConsoleCommands::parseGoto(words);
        REQUIRE(destination.has_value());
        CHECK_FALSE(destination->offlineOnly());
        CHECK((destination->level || destination->position));
    }
}

TEST_CASE("list maps names the Legends maps installed on a line per platform",
          "[dev-console][commands][legends]") {
    CHECK(ConsoleCommands::legendsMapLines(LegendsPlatform::N64, {}).empty());
    const std::vector<std::string> names{"castle", "forest"};
    const auto lines = ConsoleCommands::legendsMapLines(LegendsPlatform::Dreamcast, names);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("Dreamcast"));
    CHECK(lines[0].contains("castle, forest"));
    CHECK(lines[0].contains("goto"));
}

TEST_CASE("goto's usage reads as it does today", "[dev-console][commands]") {
    CHECK(ConsoleCommands::gotoUsage() == kGotoUsage);
}

TEST_CASE("goto refuses the words it cannot read as it does today", "[dev-console][commands]") {
    for (const Args& words : {Args{}, Args{"a", "b"}, Args{"n64"}, Args{"G1", "1", "x", "3"}}) {
        CAPTURE(words);
        const auto destination = ConsoleCommands::parseGoto(words);
        CHECK_FALSE(destination.has_value());
        if (!destination.has_value()) {
            CHECK(destination.error() == kGotoUsage);
        }
    }
}

TEST_CASE("the Legends listings read as they do today", "[dev-console][commands]") {
    const std::vector<std::string> names{"castle", "forest"};
    CHECK(ConsoleCommands::legendsMapLines(LegendsPlatform::Dreamcast, names) ==
          std::vector<std::string>{"Dreamcast Legends maps (goto <name>): castle, forest"});
    CHECK(ConsoleCommands::legendsMapLines(LegendsPlatform::N64, names) ==
          std::vector<std::string>{"N64 Legends maps (goto <name>): castle, forest"});
}

} // namespace
