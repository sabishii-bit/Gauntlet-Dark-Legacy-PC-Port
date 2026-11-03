#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/app/LevelDebugOverlay.h"
#include "game/app/ViewConsoleCommands.h"
#include "game/camera/FreeCamera.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The view's commands over state of the test's own, with or without a scene in play. */
struct View {
    LevelDebugOverlay levelDebug;
    bool hudHidden = false;
    FreeCamera freeCamera;
    std::optional<WorldCamera> inPlay; ///< where the scene in play looks from, when one is
    s32 linesAsked = 0;
    DevConsole console;

    View() {
        registerViewConsoleCommands(console.host(),
                                    {.levelDebug = levelDebug,
                                     .hudHidden = hudHidden,
                                     .freeCamera = freeCamera,
                                     .view = [this] { return inPlay; },
                                     .levelDebugLines =
                                         [this] {
                                             ++linesAsked;
                                             return std::vector<std::string>{"a line"};
                                         }});
    }

    std::string say(std::string_view line) { return test::said(console, line); }
};
} // namespace

TEST_CASE("debug_info and hidehud toggle what this machine shows", "[dev-console]") {
    View fixture;
    CHECK(fixture.say("debug_info") == "Level debug overlay on");
    CHECK(fixture.levelDebug.visible());
    CHECK(fixture.linesAsked == 1); // its lines go to the log as it comes on
    CHECK(fixture.say("debug_info") == "Level debug overlay off");
    CHECK_FALSE(fixture.levelDebug.visible());
    CHECK(fixture.linesAsked == 1);
    CHECK(fixture.say("hidehud") == "HUD hidden");
    CHECK(fixture.hudHidden);
    CHECK(fixture.say("HIDEHUD") == "HUD shown");
    CHECK_FALSE(fixture.hudHidden);
    for (const std::string_view name : {"debug_info", "hidehud", "freecam"}) {
        const ConsoleCommand* command = fixture.console.commands().find(name);
        REQUIRE(command != nullptr);
        CHECK(command->reach() == ConsoleReach::Local);
    }
    CHECK(fixture.console.commands().find("debug_info")->summary() ==
          "shows or hides the level debug overlay");
    CHECK(fixture.console.commands().find("hidehud")->summary() ==
          "hides or shows every HUD element");
    CHECK(fixture.console.commands().find("freecam")->summary() ==
          "lets the camera off the party to fly; you stand still");
}

TEST_CASE("freecam takes off from the view in play and lands again", "[dev-console]") {
    View fixture;
    CHECK(fixture.say("freecam") == "Nothing is in play.");
    CHECK_FALSE(fixture.freeCamera.active());
    WorldCamera from;
    from.position = Vec3{1.0f, 2.0f, 3.0f};
    fixture.inPlay = from;
    CHECK(fixture.say("freecam") ==
          test::shownAs("Free camera on: W A S D fly, Q and E rise and sink, the right mouse "
                        "button held turns, Shift slows"));
    CHECK(fixture.freeCamera.active());
    CHECK(fixture.freeCamera.camera().position == from.position);
    CHECK(fixture.say("freecam") == "Free camera off");
    CHECK_FALSE(fixture.freeCamera.active());
    // Landing needs nothing in play.
    fixture.freeCamera.start(from);
    fixture.inPlay.reset();
    CHECK(fixture.say("freecam") == "Free camera off");
}
