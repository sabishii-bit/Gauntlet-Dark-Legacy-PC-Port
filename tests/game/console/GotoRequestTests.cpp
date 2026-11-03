#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "game/console/ConsoleUsage.h"
#include "game/console/GotoRequest.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Args = std::vector<std::string>;

/** What goto answers words it cannot read, every form and the note. */
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]            travels to a level, standing at a point when given\n"
    "goto x y z                    moves the party to that point here\n"
    "goto <legends map> [preview]  walks an installed Legends map, or flies through it\n"
    "goto n64|dreamcast <map>      the same, naming the map's platform\n"
    "goto back                     returns from a Legends map to the level left for it\n"
    "list maps names the levels and the Legends maps installed.";

TEST_CASE("goto takes a map, a point, or a point in a map", "[dev-console][commands]") {
    auto level = parseGoto(Args{"g1"});
    REQUIRE(level.has_value());
    CHECK(level->level == "G1");
    CHECK_FALSE(level->position.has_value());
    auto point = parseGoto(Args{"10", "2.5", "-3"});
    REQUIRE(point.has_value());
    CHECK_FALSE(point->level.has_value());
    REQUIRE(point->position.has_value());
    CHECK(point->position->x == Approx(10.0f));
    CHECK(point->position->y == Approx(2.5f));
    CHECK(point->position->z == Approx(-3.0f));
    auto both = parseGoto(Args{"A1", "1", "2", "3"});
    REQUIRE(both.has_value());
    CHECK(both->level == "A1");
    CHECK(both->position == Vec3{1, 2, 3});
    auto tower = parseGoto(Args{"tower"});
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    const auto none = parseGoto(Args{});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error() == sharedGotoUsage());
    CHECK_FALSE(parseGoto(Args{"a", "b"}).has_value());
    CHECK_FALSE(parseGoto(Args{"1", "2"}).has_value());
    CHECK_FALSE(parseGoto(Args{"G1", "1", "x", "3"}).has_value());
}

TEST_CASE("a word names a map the way the catalogue does", "[dev-console][commands]") {
    CHECK(looksLikeMap("e2"));
    CHECK(looksLikeMap("S10"));
    CHECK(looksLikeMap("tower"));
    CHECK_FALSE(looksLikeMap("E1XLNSNC_CNDL01"));
    CHECK_FALSE(looksLikeMap("gru"));
    CHECK_FALSE(looksLikeMap("1"));
}

TEST_CASE("goto takes a Legends map by name or platform, a preview of it, and the way back",
          "[dev-console][commands][legends]") {
    auto named = parseGoto(Args{"n64", "Gauntlet"});
    REQUIRE(named.has_value());
    CHECK(named->legends == "Gauntlet"); // the case kept: it names a folder
    CHECK(named->platform == "n64");
    CHECK_FALSE(named->preview);
    CHECK_FALSE(named->back);
    CHECK_FALSE(named->level.has_value());
    CHECK_FALSE(named->position.has_value());
    auto preview = parseGoto(Args{"DC", "castle", "Preview"});
    REQUIRE(preview.has_value());
    CHECK(preview->legends == "castle");
    CHECK(preview->platform == "dreamcast");
    CHECK(preview->preview);
    // A platform is named in any case, and read as one word.
    for (const auto& [word, platform] : {std::pair{"DreamCast", "dreamcast"},
                                         std::pair{"dc", "dreamcast"}, std::pair{"N64", "n64"}}) {
        CAPTURE(word);
        const auto request = parseGoto(Args{word, "castle"});
        REQUIRE(request.has_value());
        CHECK(request->platform == platform);
    }
    auto bare = parseGoto(Args{"castle"});
    REQUIRE(bare.has_value());
    CHECK(bare->legends == "castle");
    CHECK_FALSE(bare->platform.has_value());
    CHECK_FALSE(bare->level.has_value());
    auto barePreview = parseGoto(Args{"castle", "preview"});
    REQUIRE(barePreview.has_value());
    CHECK(barePreview->legends == "castle");
    CHECK(barePreview->preview);
    auto back = parseGoto(Args{"back"});
    REQUIRE(back.has_value());
    CHECK(back->back);
    CHECK_FALSE(back->legends.has_value());
    // A level's name is still a level; a platform alone, or with a point, is nothing.
    const auto realmLevel = parseGoto(Args{"e2"});
    REQUIRE(realmLevel.has_value());
    CHECK(realmLevel->level == "E2");
    const auto tower = parseGoto(Args{"Tower"});
    REQUIRE(tower.has_value());
    CHECK(tower->level == "TOWER");
    CHECK_FALSE(parseGoto(Args{"n64"}).has_value());
    CHECK_FALSE(parseGoto(Args{"n64", "back"}).has_value());
    CHECK_FALSE(parseGoto(Args{"gamecube", "castle"}).has_value());
    const auto point = parseGoto(Args{"n64", "1", "2", "3"});
    REQUIRE_FALSE(point.has_value());
    CHECK(point.error() == sharedGotoUsage());
}

TEST_CASE("goto's Legends forms never reach the shared game; the rest name a level or a point",
          "[dev-console][commands][online-console]") {
    for (const Args& words : {Args{"back"}, Args{"return"}, Args{"n64", "castle"},
                              Args{"castle", "preview"}, Args{"MISSING"}, Args{"lab"}}) {
        CAPTURE(words);
        const auto destination = parseGoto(words);
        REQUIRE(destination.has_value());
        CHECK(destination->offlineOnly());
    }
    for (const Args& words :
         {Args{"g1"}, Args{"tower"}, Args{"10", "2.5", "-3"}, Args{"A1", "1", "2", "3"}}) {
        CAPTURE(words);
        const auto destination = parseGoto(words);
        REQUIRE(destination.has_value());
        CHECK_FALSE(destination->offlineOnly());
        CHECK((destination->level || destination->position));
    }
}

TEST_CASE("the shared game's goto usage reads as it does today", "[dev-console][commands]") {
    CHECK(sharedGotoUsage() == kGotoUsage);
    // goto's own forms come first, those of the Legends maps after them, under their note.
    std::vector<std::pair<std::string_view, std::string_view>> rows(GotoRequest::kUsageRows.begin(),
                                                                    GotoRequest::kUsageRows.end());
    rows.insert(rows.end(), GotoRequest::kLegendsUsageRows.begin(),
                GotoRequest::kLegendsUsageRows.end());
    const std::vector<std::string_view> notes{GotoRequest::kLegendsUsageNote};
    CHECK(consoleUsage(rows, notes) == kGotoUsage);
}

TEST_CASE("goto refuses the words it cannot read as it does today", "[dev-console][commands]") {
    for (const Args& words : {Args{}, Args{"a", "b"}, Args{"n64"}, Args{"G1", "1", "x", "3"}}) {
        CAPTURE(words);
        const auto destination = parseGoto(words);
        CHECK_FALSE(destination.has_value());
        if (!destination.has_value()) {
            CHECK(destination.error() == kGotoUsage);
        }
    }
}

} // namespace
