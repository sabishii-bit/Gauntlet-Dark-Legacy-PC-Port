#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/app/OnlineConsoleRoute.h"

namespace {
using namespace gdl::game;
using gdl::usize;
using Kind = OnlineConsoleRouting::Kind;

/** What goto answers words it cannot read. */
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]            travels to a level, standing at a point when given\n"
    "goto x y z                    moves the party to that point here\n"
    "goto <legends map> [preview]  walks an installed Legends map, or flies through it\n"
    "goto n64|dreamcast <map>      the same, naming the map's platform\n"
    "goto back                     returns from a Legends map to the level left for it\n"
    "list maps names the levels and the Legends maps installed.";

/** The refusal of a Legends map's words. */
constexpr std::string_view kLegendsOffline = "Legends maps are offline only.";

/** How the online console routes `line`, its first word naming the command. */
OnlineConsoleRouting route(std::string_view line) {
    std::vector<std::string> words;
    usize at = 0;
    while (at < line.size()) {
        const usize start = line.find_first_not_of(' ', at);
        if (start == std::string_view::npos) {
            break;
        }
        const usize end = std::min(line.find(' ', start), line.size());
        words.emplace_back(line.substr(start, end - start));
        at = end;
    }
    REQUIRE_FALSE(words.empty());
    return routeOnlineConsole(words.front(), std::span<const std::string>{words}.subspan(1));
}

/** Why the online console refuses `line`. */
std::string refusal(std::string_view line) {
    const OnlineConsoleRouting routing = route(line);
    CHECK(routing.kind == Kind::Refused);
    return routing.text;
}

/** The line the online console asks the shared game to run for `line`. */
std::string request(std::string_view line) {
    const OnlineConsoleRouting routing = route(line);
    CHECK(routing.kind == Kind::Requested);
    return routing.text;
}
} // namespace

TEST_CASE("the online console runs its own view's commands and get info here", "[online-console]") {
    for (const std::string_view line :
         {"console_permissions 1", "help", "help me", "list", "list maps", "list items G1",
          "debug_info", "hidehud", "freecam", "get info", "get info enemy#3"}) {
        CAPTURE(line);
        const OnlineConsoleRouting routing = route(line);
        CHECK(routing.kind == Kind::Local);
        CHECK(routing.text.empty());
    }
}

TEST_CASE("the online console refuses the words the shared game cannot read", "[online-console]") {
    CHECK(refusal("god x") == "Usage: god");
    CHECK(refusal("notarget now") == "Usage: notarget");
    CHECK(refusal("noclip 1") == "Usage: noclip");
    CHECK(refusal("suicide x") == "Usage: suicide");
    CHECK(refusal("unlock_tower please") == "Usage: unlock_tower");
    CHECK(refusal("goto lab") == kLegendsOffline);
    CHECK(refusal("goto back") == kLegendsOffline);
    CHECK(refusal("goto n64 castle") == kLegendsOffline);
    CHECK(refusal("goto ZZ9") == kLegendsOffline);
    CHECK(refusal("goto") == kGotoUsage);
    CHECK(refusal("goto 1 2") == kGotoUsage);
    CHECK(refusal("get") == "get <item> [amount]   keys, potions, gold, health or a powerup\n"
                            "get exp <points>      experience to add\n"
                            "get level <level>     the level to be, 1 to 99\n"
                            "get info [$SELECTED]  what the thing clicked in the view is\n"
                            "list items names the items.");
    CHECK(refusal("get banana") == "No item named banana; list items names them");
    CHECK(refusal("get exp 0") == "get exp <points>: how much to add");
    CHECK(
        refusal("spawn") ==
        "spawn <enemy> [tier] [count] [map]            so many of the swarm's kind, at a tier\n"
        "spawn generator <enemy>|special [tier] [map]  a generator breeding that kind, or the "
        "temple's or underworld's mixed brood\n"
        "spawn golem|general|gargoyle [form] [map]     a great one; a gargoyle's form is eagl, "
        "lion or serp\n"
        "spawn item <name>                             a pickup of this level, by its record\n"
        "spawn object <name> [map]                     a copy of a level's object\n"
        "A map (E2) brings the kind's voices, a great one's costume or the object from that level; "
        "list enemies, critters, items and objects name what there is.");
    CHECK(refusal("spawn gru 4") == "A tier is 1 to 3");
    CHECK(refusal("spawn enemy zz") == "No enemy kind named ZZ; list enemies names them");
}

TEST_CASE("the online console asks for a shared verb in the words it was typed in",
          "[online-console]") {
    CHECK(request("god") == "god");
    CHECK(request("notarget") == "notarget");
    CHECK(request("noclip") == "noclip");
    CHECK(request("suicide") == "suicide");
    CHECK(request("unlock_tower") == "unlock_tower");
    CHECK(request("get Gold 5") == "get Gold 5");
    CHECK(request("get exp 10") == "get exp 10");
    CHECK(request("spawn Gru 2  3") == "spawn Gru 2 3");
    CHECK(request("spawn object Polysurf G1") == "spawn object Polysurf G1");
    CHECK(request("goto g1 1 2 3") == "goto g1 1 2 3");
    CHECK(request("goto Tower") == "goto Tower");
    CHECK(request("goto 1.5 -2 3") == "goto 1.5 -2 3");
}
