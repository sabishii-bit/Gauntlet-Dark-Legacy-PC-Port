#include "game/app/ViewConsoleCommands.h"

#include <format>
#include <memory>

#include "engine/core/Log.h"

#include "game/app/LevelDebugOverlay.h"
#include "game/camera/FreeCamera.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleReach.h"
#include "game/console/DevConsole.h"

namespace gdl::game {

namespace {
using Arguments = ConsoleCommand::Arguments;

void debugInfo(const ViewConsoleAccess& access, DevConsole& console) {
    access.levelDebug.toggle();
    console.print(access.levelDebug.visible() ? "Level debug overlay on"
                                              : "Level debug overlay off");
    if (access.levelDebug.visible()) {
        // Nothing is said with nothing in play: the overlay has no lines then.
        for (const std::string& line : access.levelDebugLines()) {
            log::info("Level debug: {}", line);
        }
    }
}

void hideHud(const ViewConsoleAccess& access, DevConsole& console) {
    access.hudHidden = !access.hudHidden;
    console.print(access.hudHidden ? "HUD hidden" : "HUD shown");
}

void freeCam(const ViewConsoleAccess& access, DevConsole& console) {
    if (access.freeCamera.active()) {
        access.freeCamera.stop();
        console.print("Free camera off");
        return;
    }
    const std::optional<WorldCamera> view = access.view();
    if (!view.has_value()) {
        console.print("Nothing is in play.");
        return;
    }
    access.freeCamera.start(*view);
    console.print(std::format("Free camera on: {}", FreeCamera::kHelp));
}
} // namespace

void registerViewConsoleCommands(ConsoleHost& host, const ViewConsoleAccess& access) {
    host.add(std::make_unique<LambdaCommand>(
        "debug_info", "shows or hides the level debug overlay", ConsoleReach::Local,
        [access](DevConsole& console, Arguments /*arguments*/) { debugInfo(access, console); }));
    host.add(std::make_unique<LambdaCommand>(
        "hidehud", "hides or shows every HUD element", ConsoleReach::Local,
        [access](DevConsole& console, Arguments /*arguments*/) { hideHud(access, console); }));
    host.add(std::make_unique<LambdaCommand>(
        "freecam", "lets the camera off the party to fly; you stand still", ConsoleReach::Local,
        [access](DevConsole& console, Arguments /*arguments*/) { freeCam(access, console); }));
}

} // namespace gdl::game
