#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/console/FixtureRequest.h"
#include "game/scenes/FixtureConsoleCommands.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayedLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

/** What activate answers words it cannot read. */
constexpr std::string_view kActivateUsage =
    "activate $SELECTED     fires the switch selected, as if you stood on it\n"
    "activate switch#N      the same, by the token list switches gives\n"
    "activate <id or name>  the same, by a switch's id, its name or its target's\n"
    "The camera follows what it opens, as it does for a player.";

/** The fixture commands over a level of the test's own. */
struct Fixtures {
    std::unique_ptr<test::PlayedLevel> level;
    DevConsole console;

    explicit Fixtures(std::unique_ptr<test::PlayedLevel> played) : level(std::move(played)) {
        registerFixtureConsoleCommands(console.host(),
                                       {.scene = [this] { return level->openScene(); },
                                        .world = [this] { return level->openWorld(); }});
    }

    std::string say(std::string_view line) { return test::said(console, line); }
    Words words(std::string_view command, const Words& arguments) const {
        const ConsoleCommand* found = console.commands().find(command);
        REQUIRE(found != nullptr);
        return found->completions(arguments);
    }
};

} // namespace

TEST_CASE("with nothing in play each fixture command answers in its own words", "[dev-console]") {
    Fixtures fixture{std::make_unique<test::PlayedLevel>()};
    CHECK(fixture.say("activate") == kActivateUsage);
    CHECK(fixture.say("activate switch#0") == "Nothing is in play.");
    CHECK(
        fixture.say("lock") ==
        "lock $SELECTED|gate#N|chest#N|<name>  a door shut again, or a chest wanting a key again\n"
        "lock character <code>                 a character hidden again; the starting eight stay\n"
        "list characters names the codes; list switches and get info name the rest.");
    CHECK(fixture.say("lock chest#0") == "Nothing is in play.");
    CHECK(fixture.say("unlock gate#0") == "Nothing is in play.");
    // A character's code is the profile's form; without it the commands say how they are used.
    CHECK(fixture.say("lock character MIN") == fixture.say("lock"));
    CHECK(fixture.say("unlock CHARACTER MIN") == fixture.say("unlock"));
    CHECK(fixture.words("activate", Words{"s"}).empty());
    CHECK(fixture.words("lock", Words{"c"}) == Words{"character"});
    CHECK(fixture.words("unlock", Words{"c"}) == Words{"character"});
    CHECK(fixture.words("lock", Words{"character", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    CHECK(fixture.words("lock", Words{"CHARACTER", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    // character in any other case is not completed.
    CHECK(fixture.words("lock", Words{"Character", "M"}).empty());
    CHECK(fixture.words("lock", Words{"character", "M", "x"}).empty());
    for (const std::string_view name : {"activate", "unlock", "lock"}) {
        const ConsoleCommand* command = fixture.console.commands().find(name);
        REQUIRE(command != nullptr);
        CHECK(command->reach() == ConsoleReach::OfflineOnly);
    }
    CHECK(fixture.console.commands().find("activate")->summary() ==
          "$SELECTED, switch#N, an id or a name: fires that switch as if stood on");
    CHECK(fixture.console.commands().find("unlock")->summary() ==
          "a door or chest without a key, or character <code> for the select screen");
    CHECK(fixture.console.commands().find("lock")->summary() ==
          "a door shut or a chest wanting a key again, or character <code> hidden again");
}

TEST_CASE("every fixture command answers in a level in its own words", "[dev-console][assets]") {
    Fixtures fixture{std::make_unique<test::PlayedLevel>("G2")};
    PlayScene& scene = fixture.level->scene;
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));
    const auto reopen = [&] { REQUIRE(scene.restoreLocal(opened)); };

    // The level's switches, as its triggers stand, are its layout's.
    const std::vector<SwitchEntry> switches = switchesInPlay(fixture.level->world);
    const std::vector<SwitchEntry> laidOut = switchesOf(fixture.level->world.layout());
    REQUIRE(switches.size() == 31);
    REQUIRE(laidOut.size() == switches.size());
    for (usize i = 0; i < switches.size(); ++i) {
        CAPTURE(i);
        CHECK(switches[i].index == laidOut[i].index);
        CHECK(switches[i].id == laidOut[i].id);
        CHECK(switches[i].name == laidOut[i].name);
        CHECK(switches[i].target == laidOut[i].target);
    }
    CHECK(switches[0].target == "G2ELEV7");
    CHECK(switches[30].name == "TRIGGERS");

    // The level's switches, doors and chests, by the words the console offers for them.
    CHECK(fixture.words("activate", Words{"s"}) ==
          Words{"switch#0",  "BRIDGEPAD", "switch#1",  "BRIDGEPAD", "switch#2",  "BRIDGEPAD",
                "switch#3",  "BRIDGEPAD", "switch#4",  "BRIDGEPAD", "switch#5",  "BRIDGEPAD",
                "switch#6",  "BRIDGEPAD", "switch#7",  "BRIDGEPAD", "switch#8",  "BRIDGEPAD",
                "switch#9",  "BRIDGEPAD", "switch#10", "BRIDGEPAD", "switch#11", "BRIDGEPAD",
                "switch#12", "BRIDGEPAD", "switch#13", "BRIDGEPAD", "switch#14", "BRIDGEPAD",
                "switch#15", "BRIDGEPAD", "switch#16", "BRIDGEPAD", "switch#17", "TRIGGER3",
                "switch#18", "TRIGGER3",  "switch#19", "TRIGGER3",  "switch#20", "TRIGGER3",
                "switch#21", "TRIGGER3",  "switch#22", "TRIGGER3",  "switch#23", "TRIGGER3",
                "switch#24", "TRIGGER2",  "switch#25", "TRIGGER2",  "switch#26", "TRIGGER2",
                "switch#27", "TRIGGER2",  "switch#28", "TRIGGER2",  "switch#29", "TRIGGER2",
                "switch#30", "TRIGGERS"});
    CHECK(fixture.words("lock", Words{"c"}) ==
          Words{"character", "gate#0",   "GATEB",    "gate#1",   "GATEB",    "gate#2",   "GATED",
                "chest#0",   "CHEST",    "chest#1",  "CHEST",    "chest#2",  "CHEST",    "chest#4",
                "CHEST",     "chest#6",  "CHEST",    "chest#7",  "CHEST",    "chest#11", "CHEST",
                "chest#13",  "CHEST",    "chest#14", "CHEST",    "chest#16", "CHEST",    "chest#17",
                "CHEST",     "chest#18", "CHEST",    "chest#20", "CHESTG1",  "chest#21", "CHESTG3",
                "chest#22",  "CHESTG2",  "chest#24", "CHESTG4",  "chest#25", "CHESTG0",  "chest#26",
                "CHESTEXP",  "chest#27", "CHESTEXP"});
    CHECK(fixture.words("lock", Words{"character", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    CHECK(fixture.words("lock", Words{"Character", "M"}).empty());
    // With chests shown, c completes to character first.
    CHECK(test::suggestionAfter(fixture.console, "lock c") == "haracter");
    CHECK(test::suggestionAfter(fixture.console, "unlock ch") == "aracter");
    CHECK(test::suggestionAfter(fixture.console, "lock chest#1") == "1");
    CHECK(fixture.say("activate") == kActivateUsage);
    CHECK(fixture.say("activate NOPE") == "NOPE names no switch here; list switches names "
                                          "them, or click a pad and activate $SELECTED");
    CHECK(
        fixture.say("activate 21") ==
        "21 names no switch here; list switches names them, or click a pad and activate $SELECTED");
    CHECK(fixture.say("activate BRIDGEPAD") ==
          "BRIDGEPAD names 17 switches: switch#0, switch#1, switch#2, switch#3, switch#4, "
          "switch#5, switch#6, switch#7, switch#8,\n"
          "switch#9, switch#10, switch#11, switch#12, switch#13, switch#14, switch#15, switch#16; "
          "say which by its token");
    CHECK(fixture.say("activate G2ELEV8") ==
          "G2ELEV8 names 3 switches: switch#11, switch#19, switch#20; say which by its token");
    CHECK(fixture.say("activate switch#0") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture.say("activate switch#0") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture.say("activate switch#1") == "Pressed switch#1 BRIDGEPAD");
    CHECK(fixture.say("activate switch#23") ==
          "switch#23 TRIGGER3 is not for this party, or stands open for good");
    CHECK(fixture.say("activate g2elev7") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture.say("activate triggers") ==
          "Pressed switch#30 TRIGGERS: G2BAR_WALL answers, the camera following");
    reopen();
    CHECK(fixture.say("unlock NOPE") ==
          "NOPE names no door or chest here; gate#N, chest#N, a name or $SELECTED after a click "
          "does");
    CHECK(fixture.say("unlock GATEB") ==
          "GATEB names 2 things: gate#0, gate#1; say which by its token");
    CHECK(fixture.say("unlock gate#0") == "gate#0 GATEB unlocked: it swings open");
    CHECK(fixture.say("unlock gate#0") == "gate#0 GATEB is open already");
    CHECK(fixture.say("lock gate#0") == "gate#0 GATEB locked: shut until a key is spent");
    CHECK(fixture.say("lock gate#0") == "gate#0 GATEB is shut already");
    CHECK(fixture.say("unlock gated") == "gate#2 GATED unlocked: it swings open");
    CHECK(fixture.say("unlock chest#0") == "chest#0 CHEST unlocked: it opens without a key");
    CHECK(fixture.say("lock chest#0") == "chest#0 CHEST locked: a key opens it");
    CHECK(fixture.say("lock chest#0") == "chest#0 CHEST locked: a key opens it");
    CHECK(fixture.say("unlock CHESTG1") == "chest#20 CHESTG1 unlocked: it opens without a key");
    CHECK(fixture.say("lock CHEST") ==
          "CHEST names 12 things: chest#0, chest#1, chest#2, chest#4, chest#6, chest#7, chest#11, "
          "chest#13, chest#14, chest#16, chest#17,\n"
          "chest#18; say which by its token");
    reopen();
}
