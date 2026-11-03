#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/app/ConsoleActionRoute.h"
#include "game/console/DevConsole.h"
#include "game/level/LevelCatalog.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/SharedConsoleCommand.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr usize kLongLine = ConsoleAction::kMaxLine + 10; ///< a line too long for the wire
constexpr std::string_view kRequested = "Command requested for the shared game.";
constexpr std::string_view kNotQueued =
    "Command not queued: permission denied, session busy, or request too long.";

/** What get and spawn answer words they cannot read. */
constexpr std::string_view kGetUsage =
    "get <item> [amount]   keys, potions, gold, health or a powerup\n"
    "get exp <points>      experience to add\n"
    "get level <level>     the level to be, 1 to 99\n"
    "get info [$SELECTED]  what the thing clicked in the view is\n"
    "list items names the items.";
constexpr std::string_view kSpawnUsage =
    "spawn <enemy> [tier] [count] [map]            so many of the swarm's kind, at a tier\n"
    "spawn generator <enemy>|special [tier] [map]  a generator breeding that kind, or the "
    "temple's or underworld's mixed brood\n"
    "spawn golem|general|gargoyle [form] [map]     a great one; a gargoyle's form is eagl, lion "
    "or serp\n"
    "spawn item <name>                             a pickup of this level, by its record\n"
    "spawn object <name> [map]                     a copy of a level's object\n"
    "A map (E2) brings the kind's voices, a great one's costume or the object from that level; "
    "list enemies, critters, items and objects name what there is.";

/** Offline play's online answer: there is no online game. */
std::optional<bool> noOnlineGame(s32 /*seat*/, const std::string& /*line*/) {
    return std::nullopt;
}

/** What a fresh console shows of `text`, its lines joined by breaks. */
std::string shownAs(std::string_view text) {
    DevConsole console;
    console.print(text);
    std::string shown;
    for (const std::string& line : console.output()) {
        shown += shown.empty() ? "" : "\n";
        shown += line;
    }
    return shown;
}

/** What `console` shows under its echo of `line` once it has run it. */
std::string said(DevConsole& console, std::string_view line) {
    const usize before = console.output().size();
    console.run(line);
    std::string shown;
    for (usize i = before + 1; i < console.output().size(); ++i) {
        shown += shown.empty() ? "" : "\n";
        shown += console.output()[i];
    }
    return shown;
}
} // namespace

TEST_CASE("offline the route holds the console's lines for its seat until the step takes them",
          "[dev-console][online-console]") {
    ConsoleActionRoute route{noOnlineGame, [] { return 1; }};
    CHECK(route.submit("god") == std::nullopt);
    CHECK(route.submit("get Gold 5") == std::nullopt);
    CHECK(route.takeOffline() == std::vector<ConsoleAction>{{.seat = 1, .line = "god"},
                                                            {.seat = 1, .line = "get Gold 5"}});
    CHECK(route.takeOffline().empty());
    // A line longer than the wire carries is held whole: offline play never sends it.
    const std::string longLine = "god" + std::string(kLongLine - 3, ' ');
    CHECK(route.submit(longLine) == std::nullopt);
    const std::vector<ConsoleAction> held = route.takeOffline();
    REQUIRE(held.size() == 1);
    CHECK(held.front().line == longLine);
    CHECK(held.front().admitted());
    CHECK_FALSE(held.front().valid());
}

TEST_CASE("offline with nobody in the route says so and holds nothing",
          "[dev-console][online-console]") {
    ConsoleActionRoute route{noOnlineGame, [] { return -1; }};
    CHECK(route.submit("god") == "Nobody is in.");
    CHECK(route.takeOffline().empty());
}

TEST_CASE("online the route asks the shared game for the console's seat and says what it did",
          "[dev-console][online-console]") {
    std::vector<std::pair<s32, std::string>> asked;
    bool queued = true;
    s32 seat = 2;
    ConsoleActionRoute route{[&](s32 forSeat, std::string line) -> std::optional<bool> {
                                 asked.emplace_back(forSeat, std::move(line));
                                 return queued;
                             },
                             [&] { return seat; }};
    CHECK(route.submit("god") == kRequested);
    queued = false;
    CHECK(route.submit("spawn Gru 2 3") == kNotQueued);
    // With nobody in, the online game is still the one asked; it queues nothing for no seat.
    seat = -1;
    CHECK(route.submit("god") == kNotQueued);
    CHECK(asked ==
          std::vector<std::pair<s32, std::string>>{{2, "god"}, {2, "spawn Gru 2 3"}, {-1, "god"}});
    CHECK(route.takeOffline().empty());
}

TEST_CASE("offline with nobody in every shared verb says so once its words are read",
          "[dev-console][online-console]") {
    DevConsole console;
    const LevelCatalog levels;
    ConsoleActionRoute route{noOnlineGame, [] { return -1; }};
    registerSharedConsoleCommands(console.host(), console.commands(), route, levels);
    // Words a verb cannot read are answered first, as they are with somebody in.
    CHECK(said(console, "get") == shownAs(kGetUsage));
    CHECK(said(console, "get banana") == "No item named banana; list items names them");
    CHECK(said(console, "spawn") == shownAs(kSpawnUsage));
    CHECK(said(console, "spawn gru 4") == "A tier is 1 to 3");
    for (const std::string_view line :
         {"god", "notarget", "noclip", "get gold 5", "spawn gru", "unlock_tower", "suicide",
          "goto 1 2 3", "goto G1", "goto tower", "god x"}) {
        CAPTURE(line);
        CHECK(said(console, line) == "Nobody is in.");
    }
    CHECK(route.takeOffline().empty());
}
