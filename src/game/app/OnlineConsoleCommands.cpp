#include "game/app/OnlineConsoleCommands.h"

#include <memory>

#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleReach.h"
#include "game/console/DevConsole.h"

namespace gdl::game {

void registerOnlineConsoleCommands(ConsoleHost& host, const OnlineConsoleAccess& access) {
    host.add(std::make_unique<LambdaCommand>(
        "console_permissions", "0 or 1: host allows guests to use the console",
        ConsoleReach::HostOnly, [access](DevConsole& console, ConsoleCommand::Arguments arguments) {
            if (arguments.size() != 1 || (arguments[0] != "0" && arguments[0] != "1")) {
                console.print("Usage: console_permissions 0 or 1");
            } else if (access.permitGuests(arguments[0] == "1")) {
                console.print(arguments[0] == "1" ? "Guest consoles enabled"
                                                  : "Guest consoles disabled");
            } else {
                console.print("Only the online host can change console permissions.");
            }
        }));
}

} // namespace gdl::game
