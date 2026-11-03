#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/app/ProfileConsoleCommands.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/console/ConsoleListings.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/console/LevelListingLines.h"
#include "game/scenes/PlayedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** What the stand-in lock and unlock say when the words are not a character's. */
constexpr std::string_view kStandIn = "a door or chest";

/** The profile's words over a level of the test's own and saves in a scratch folder, beside
 * stand-ins for the lock and unlock commands the fixtures own. */
struct Profile {
    std::unique_ptr<test::PlayedLevel> level;
    SaveSlots saves;
    ConsoleListings listings;
    DevConsole console;

    explicit Profile(std::unique_ptr<test::PlayedLevel> played) : level(std::move(played)) {
        REQUIRE(saves.open(test::scratchDirectory("profile-console-saves"), 4));
        for (const std::string_view name : {"unlock", "lock"}) {
            console.addCommand(std::string(name), "stands in", ConsoleReach::OfflineOnly,
                               [](DevConsole& shown, DevConsole::Arguments /*arguments*/) {
                                   shown.print(kStandIn);
                               });
        }
        console.addCommand(listCommand(listings));
        registerProfileConsoleCommands(console.host(), listings,
                                       {.saves = saves, .config = level->config, .scene = [this] {
                                            return level->openScene();
                                        }});
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("the profile's words offer and hide characters by their codes", "[dev-console]") {
    Profile fixture{std::make_unique<test::PlayedLevel>()};
    CHECK(fixture.say("unlock character XYZ") ==
          "No character is coded XYZ; list characters names them");
    CHECK(fixture.say("unlock character WAR") == "WAR is always there to choose");
    const std::string code{classCode(kStartingClassCount)};
    CHECK(fixture.say("unlock character " + code) == "MIN unlocked: the select screen offers it");
    CHECK((fixture.saves.classUnlocks() & 1U) != 0);
    CHECK(fixture.say("list characters") ==
          test::printed(characterLines(fixture.saves.classUnlocks())));
    CHECK(fixture.say("lock CHARACTER " + code) == "MIN locked: the select screen hides it again");
    CHECK((fixture.saves.classUnlocks() & 1U) == 0);
    CHECK(fixture.say("list characters") ==
          test::printed(characterLines(fixture.saves.classUnlocks())));
    // Words that are not a character's are the command's own.
    CHECK(fixture.say("lock chest#0") == kStandIn);
    CHECK(fixture.say("unlock character") == kStandIn);
    CHECK(fixture.say("unlock character MIN x") == kStandIn);
    // Online the forms are refused as the commands are.
    fixture.console.setSession({.seat = ConsoleSeat::Guest});
    CHECK(fixture.say("unlock character " + code) == ConsolePolicy::kRefused);
    CHECK((fixture.saves.classUnlocks() & 1U) == 0);
    CHECK(fixture.listings.words() == std::vector<std::string>{"characters"});
    CHECK(fixture.listings.usage() ==
          "list characters  every playable character, locked or hidden too");
}

TEST_CASE("hiding a character again takes it from the party in play", "[dev-console][assets]") {
    Profile fixture{std::make_unique<test::PlayedLevel>("G2")};
    const std::string code{classCode(kStartingClassCount)};
    fixture.level->save(0).classUnlock = 1U;
    CHECK(fixture.say("lock character " + code) == "MIN locked: the select screen hides it again");
    CHECK(fixture.level->save(0).classUnlock == 0);
}
