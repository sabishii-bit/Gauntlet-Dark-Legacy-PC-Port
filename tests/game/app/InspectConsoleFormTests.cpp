#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/app/InspectConsoleForm.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/scenes/SceneSelection.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

/** get info's form on a stand-in get, over a selection and a description of the test's own. */
struct Inspect {
    std::optional<SceneSelection> selected;
    std::vector<std::string> described; ///< the tokens described, in turn
    DevConsole console;

    Inspect() {
        console.addCommand(
            "get", "stands in", ConsoleReach::Local,
            [](DevConsole& shown, DevConsole::Arguments /*arguments*/) { shown.print("got"); });
        console.addForm("get", std::make_unique<InspectConsoleForm>(InspectConsoleAccess{
                                   .selected = [this] { return selected; },
                                   .describe =
                                       [this](const SceneSelection& selection) {
                                           described.push_back(selection.token());
                                           return Words{"first line", "second line"};
                                       }}));
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("get info says what the thing selected or named is", "[dev-console]") {
    Inspect fixture;
    CHECK(fixture.say("get info") ==
          test::shownAs("Nothing is selected: click something in the view while the console is "
                        "down, then get info $SELECTED"));
    CHECK(fixture.say("get info banana") ==
          "banana names nothing; $SELECTED or a token such as object#12 or enemy#3 does");
    CHECK(fixture.say("get INFO enemy#3") == "first line\nsecond line");
    fixture.selected = SceneSelection::parse("chest#2");
    REQUIRE(fixture.selected.has_value());
    CHECK(fixture.say("get info") == "first line\nsecond line");
    CHECK(fixture.described == std::vector<std::string>{"enemy#3", "chest#2"});
    // Every other word is the command's.
    CHECK(fixture.say("get gold 5") == "got");
    CHECK(fixture.say("get") == "got");
    const InspectConsoleForm form{{}};
    CHECK(form.reach() == ConsoleReach::Local);
    CHECK(form.claims(Words{"info"}));
    CHECK(form.claims(Words{"Info", "object#12"}));
    CHECK_FALSE(form.claims(Words{"gold", "5"}));
    CHECK_FALSE(form.claims(Words{}));
}
