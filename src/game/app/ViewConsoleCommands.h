#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/world/WorldCamera.h"

#include "game/console/ConsoleHost.h"

namespace gdl::game {

class FreeCamera;
class LevelDebugOverlay;

/** What the view's console commands read and change of the game, each read as one runs. */
struct ViewConsoleAccess {
    LevelDebugOverlay& levelDebug;
    bool& hudHidden;
    FreeCamera& freeCamera;
    /** Where the scene the console reads looks from; none with nothing in play. */
    std::function<std::optional<WorldCamera>()> view;
    /** The level debug overlay's lines for the level in play; none with nothing in play. */
    std::function<std::vector<std::string>()> levelDebugLines;
};

/**
 * Registers the commands over what this machine shows, which run wherever its console is
 * open: debug_info shows or hides the level debug overlay, hidehud every HUD element, and
 * freecam lets the camera off the party to fly.
 */
void registerViewConsoleCommands(ConsoleHost& host, const ViewConsoleAccess& access);

} // namespace gdl::game
