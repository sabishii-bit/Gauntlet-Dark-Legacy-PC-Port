#pragma once
#include <functional>

#include "game/console/ConsoleHost.h"

namespace gdl::game {

/** What the online game's console commands change of it. */
struct OnlineConsoleAccess {
    /** Lets the guests' consoles run commands, or not: whether this machine hosts an online
     * game that took the change. */
    std::function<bool(bool allow)> permitGuests;
};

/** Registers the commands only the online host runs: console_permissions lets the guests use
 * their consoles or not. */
void registerOnlineConsoleCommands(ConsoleHost& host, const OnlineConsoleAccess& access);

} // namespace gdl::game
