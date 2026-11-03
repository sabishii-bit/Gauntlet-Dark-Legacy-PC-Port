#include "game/app/OnlineConsoleRoute.h"

#include <format>
#include <string>
#include <utility>

#include "game/console/ConsoleCommands.h"

namespace gdl::game {

OnlineConsoleRouting routeOnlineConsole(std::string_view name,
                                        ConsoleCommand::Arguments arguments) {
    if (name == "console_permissions" || name == "help" || name == "list" || name == "debug_info" ||
        name == "hidehud" || name == "freecam") {
        return {};
    }
    std::string error;
    if (name == "get") {
        const auto gift = ConsoleCommands::parseGet(arguments, error);
        if (gift && gift->kind == ConsoleCommands::Gift::Kind::Info) {
            return {};
        }
        if (gift) {
            error.clear();
        }
    } else if (name == "goto") {
        if (const auto destination = ConsoleCommands::parseGoto(arguments, error)) {
            error = destination->offlineOnly() ? "Legends maps are offline only." : "";
        }
    } else if (name == "spawn") {
        if (ConsoleCommands::parseSpawn(arguments, error)) {
            error.clear();
        }
    } else if (!arguments.empty()) {
        error = std::format("Usage: {}", name);
    }
    if (!error.empty()) {
        return {.kind = OnlineConsoleRouting::Kind::Refused, .text = std::move(error)};
    }
    std::string line(name);
    for (const auto& argument : arguments) {
        line += ' ';
        line += argument;
    }
    return {.kind = OnlineConsoleRouting::Kind::Requested, .text = std::move(line)};
}

} // namespace gdl::game
