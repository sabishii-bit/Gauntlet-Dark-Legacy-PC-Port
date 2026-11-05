#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/app/OnlineConsoleCommands.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** console_permissions over an online game of the test's own, which keeps what it was asked. */
struct Online {
    bool hosting = true; ///< whether the made-up game takes the change
    std::vector<bool> asked;
    DevConsole console;

    Online() {
        registerOnlineConsoleCommands(console.host(), {.permitGuests = [this](bool allow) {
                                          asked.push_back(allow);
                                          return hosting;
                                      }});
        console.setSession({.role = ConsoleRole::Host});
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("console_permissions lets the guests use their consoles, on the host alone",
          "[dev-console][online-console]") {
    Online fixture;
    for (const std::string_view line :
         {"console_permissions", "console_permissions 2", "console_permissions 0 1"}) {
        CAPTURE(line);
        CHECK(fixture.say(line) == "Usage: console_permissions 0 or 1");
    }
    CHECK(fixture.asked.empty());
    CHECK(fixture.say("console_permissions 1") == "Guest consoles enabled");
    CHECK(fixture.say("console_permissions 0") == "Guest consoles disabled");
    CHECK(fixture.asked == std::vector<bool>{true, false});
    fixture.hosting = false;
    CHECK(fixture.say("console_permissions 1") ==
          "Only the online host can change console permissions.");
    // Anyone but the online host is refused before the game is asked.
    for (const ConsoleSession& session :
         {ConsoleSession{}, ConsoleSession{.role = ConsoleRole::Guest}}) {
        fixture.console.setSession(session);
        CHECK(fixture.say("console_permissions 1") == ConsolePolicy::kRefused);
    }
    CHECK(fixture.asked.size() == 3);
    const ConsoleCommand* command = fixture.console.commands().find("console_permissions");
    REQUIRE(command != nullptr);
    CHECK(command->reach() == ConsoleReach::HostOnly);
    CHECK(command->summary() == "0 or 1: host allows guests to use the console");
}
