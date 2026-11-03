#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/app/PartyCountCommand.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/scenes/PlayedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

/** party_count over a level of the test's own. */
struct PartyCount {
    std::unique_ptr<test::PlayedLevel> level;
    std::optional<s32> count;
    DevConsole console;

    explicit PartyCount(std::unique_ptr<test::PlayedLevel> played) : level(std::move(played)) {
        console.addCommand(std::make_unique<PartyCountCommand>(
            PartyCountAccess{.count = count, .scene = [this] { return level->openScene(); }}));
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("party_count sets the party size levels set themselves up for", "[dev-console]") {
    PartyCount fixture{std::make_unique<test::PlayedLevel>()};
    CHECK(fixture.say("party_count") == "The level is set up for the players present");
    CHECK(fixture.say("party_count 3") == "The level is set up for a party of 3");
    CHECK(fixture.count == 3);
    CHECK(fixture.say("party_count 5") ==
          "party_count <1-4>  sets the level up for a party that size\n"
          "party_count off    follows the players present again\n"
          "party_count        says which it is");
    CHECK(fixture.count == 3);
    CHECK(fixture.say("party_count off") == "The level is set up for the players present");
    CHECK_FALSE(fixture.count.has_value());
    const ConsoleCommand* command = fixture.console.commands().find("party_count");
    REQUIRE(command != nullptr);
    CHECK(command->reach() == ConsoleReach::OfflineOnly);
    CHECK(command->summary() == "<1-4> or off: the party size the level sets itself up for");
    CHECK(command->completions(Words{""}) == Words{"1", "2", "3", "4", "off"});
    CHECK(command->completions(Words{"3", ""}).empty());
}

TEST_CASE("party_count reaches the scene in play", "[dev-console][assets]") {
    PartyCount fixture{std::make_unique<test::PlayedLevel>("G2")};
    const PlayScene& scene = fixture.level->scene;
    CHECK(fixture.say("party_count 3") == "The level is set up for a party of 3");
    CHECK(scene.partyCountOverride() == 3);
    CHECK(fixture.say("party_count off") == "The level is set up for the players present");
    CHECK_FALSE(scene.partyCountOverride().has_value());
}
